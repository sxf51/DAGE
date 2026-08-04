//! Runtime-neutral ownership and async wrappers over the stable DAGE C ABI.
use std::ffi::{c_char, c_int, c_void};
use std::future::Future;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::pin::Pin;
use std::ptr::{null_mut, NonNull};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

const OK: c_int = 0;
const EXECUTION_ERROR: c_int = 5;
const CANCELLED: c_int = 6;
const BUFFER_TOO_SMALL: c_int = 8;

#[repr(C)]
#[derive(Clone, Copy)]
struct StringView {
    data: *const c_char,
    size: usize,
}
#[repr(C)]
struct OwnedBuffer {
    data: *const c_char,
    size: usize,
    release: Option<unsafe extern "C" fn(*const c_char, usize, *mut c_void)>,
    release_userdata: *mut c_void,
}
#[repr(C)]
struct NativeExecutionContext {
    struct_size: u32,
    run_id: StringView,
    node_id: StringView,
    node_type: StringView,
    attempt: u32,
    idempotency_key: StringView,
    run_mode: StringView,
    deadline_remaining_ms: u64,
    commit_effect: *mut c_void,
    commit_effect_userdata: *mut c_void,
    is_cancelled: *mut c_void,
    cancellation_userdata: *mut c_void,
}

type EngineRaw = *mut c_void;
type WorkflowRaw = *mut c_void;
type RunRaw = *mut c_void;
type CompletionRaw = *mut c_void;
type AsyncCallback = unsafe extern "C" fn(
    *const NativeExecutionContext,
    StringView,
    CompletionRaw,
    *mut c_void,
) -> c_int;
type DestroyCallback = unsafe extern "C" fn(*mut c_void);

#[link(name = "dage")]
extern "C" {
    fn dage_engine_create(options: *const c_void, out: *mut EngineRaw) -> c_int;
    fn dage_engine_destroy(engine: EngineRaw);
    fn dage_engine_load(engine: EngineRaw, json: StringView, out: *mut WorkflowRaw) -> c_int;
    fn dage_engine_register_async_executor(
        engine: EngineRaw,
        name: StringView,
        callback: AsyncCallback,
        userdata: *mut c_void,
        destroy: DestroyCallback,
    ) -> c_int;
    fn dage_workflow_destroy(workflow: WorkflowRaw);
    fn dage_workflow_export_mermaid(
        workflow: WorkflowRaw,
        buffer: *mut c_char,
        size: usize,
        required: *mut usize,
    ) -> c_int;
    fn dage_run_create(engine: EngineRaw, workflow: WorkflowRaw, out: *mut RunRaw) -> c_int;
    fn dage_run_destroy(run: RunRaw);
    fn dage_run_execute(
        run: RunRaw,
        input: StringView,
        output: *mut c_char,
        size: usize,
        required: *mut usize,
    ) -> c_int;
    fn dage_run_cancel_with_reason(run: RunRaw, reason: StringView) -> c_int;
    fn dage_executor_complete(
        completion: CompletionRaw,
        status: c_int,
        output: *const OwnedBuffer,
    ) -> c_int;
    fn dage_executor_abandon(completion: CompletionRaw);
    fn dage_executor_completion_is_cancelled(completion: CompletionRaw) -> u8;
    fn dage_executor_completion_commit_effect(completion: CompletionRaw) -> c_int;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub i32);
pub type Result<T> = std::result::Result<T, Error>;

fn check(status: c_int) -> Result<()> {
    if status == OK {
        Ok(())
    } else {
        Err(Error(status))
    }
}
fn view(value: &str) -> StringView {
    StringView {
        data: value.as_ptr().cast(),
        size: value.len(),
    }
}
unsafe fn string_of(value: StringView) -> String {
    if value.size == 0 {
        return String::new();
    }
    let bytes = unsafe { std::slice::from_raw_parts(value.data.cast::<u8>(), value.size) };
    String::from_utf8_lossy(bytes).into_owned()
}

struct EngineInner(NonNull<c_void>);
unsafe impl Send for EngineInner {}
unsafe impl Sync for EngineInner {}
impl Drop for EngineInner {
    fn drop(&mut self) {
        unsafe { dage_engine_destroy(self.0.as_ptr()) }
    }
}

pub struct Engine {
    inner: Arc<EngineInner>,
}
impl Engine {
    pub fn new() -> Result<Self> {
        let mut raw = null_mut();
        check(unsafe { dage_engine_create(std::ptr::null(), &mut raw) })?;
        NonNull::new(raw)
            .map(|raw| Self {
                inner: Arc::new(EngineInner(raw)),
            })
            .ok_or(Error(101))
    }
    pub fn load(&self, json: &str) -> Result<Workflow> {
        let mut raw = null_mut();
        check(unsafe { dage_engine_load(self.inner.0.as_ptr(), view(json), &mut raw) })?;
        NonNull::new(raw)
            .map(|raw| Workflow {
                raw,
                engine: self.inner.clone(),
            })
            .ok_or(Error(101))
    }
    pub fn create_run(&self, workflow: &Workflow) -> Result<Run> {
        if !Arc::ptr_eq(&self.inner, &workflow.engine) {
            return Err(Error(1));
        }
        let mut raw = null_mut();
        check(unsafe { dage_run_create(self.inner.0.as_ptr(), workflow.raw.as_ptr(), &mut raw) })?;
        let raw = NonNull::new(raw).ok_or(Error(101))?;
        Ok(Run {
            inner: Arc::new(RunInner {
                raw,
                engine: self.inner.clone(),
                active: AtomicBool::new(false),
            }),
        })
    }
    pub fn register_async_executor<F>(
        &self,
        name: &str,
        callback: F,
        spawner: Arc<dyn Spawner>,
    ) -> Result<()>
    where
        F: Fn(AsyncExecutionContext, String) -> AsyncExecutorFuture + Send + Sync + 'static,
    {
        let registration = Box::new(Registration {
            callback: Box::new(callback),
            spawner,
        });
        let userdata = Box::into_raw(registration).cast();
        // With a valid Engine/callback, the C ABI owns userdata once entered, including failures.
        check(unsafe {
            dage_engine_register_async_executor(
                self.inner.0.as_ptr(),
                view(name),
                async_thunk,
                userdata,
                destroy_registration,
            )
        })
    }
}

pub struct Workflow {
    raw: NonNull<c_void>,
    engine: Arc<EngineInner>,
}
impl Workflow {
    pub fn mermaid(&self) -> Result<String> {
        let mut required = 0usize;
        let first = unsafe {
            dage_workflow_export_mermaid(self.raw.as_ptr(), null_mut(), 0, &mut required)
        };
        if first != BUFFER_TOO_SMALL {
            return Err(Error(first));
        }
        let mut bytes = vec![0u8; required];
        check(unsafe {
            dage_workflow_export_mermaid(
                self.raw.as_ptr(),
                bytes.as_mut_ptr().cast(),
                bytes.len(),
                &mut required,
            )
        })?;
        if bytes.last() == Some(&0) {
            bytes.pop();
        }
        String::from_utf8(bytes).map_err(|_| Error(101))
    }
}
impl Drop for Workflow {
    fn drop(&mut self) {
        unsafe { dage_workflow_destroy(self.raw.as_ptr()) }
    }
}

struct RunInner {
    raw: NonNull<c_void>,
    #[allow(dead_code)]
    engine: Arc<EngineInner>,
    active: AtomicBool,
}
unsafe impl Send for RunInner {}
unsafe impl Sync for RunInner {}
impl Drop for RunInner {
    fn drop(&mut self) {
        unsafe { dage_run_destroy(self.raw.as_ptr()) }
    }
}

#[derive(Clone)]
pub struct Run {
    inner: Arc<RunInner>,
}
impl Run {
    pub fn cancel(&self, reason: &str) -> Result<()> {
        check(unsafe { dage_run_cancel_with_reason(self.inner.raw.as_ptr(), view(reason)) })
    }
    pub fn execute(&self, input: &str) -> Result<String> {
        if self.inner.active.swap(true, Ordering::AcqRel) {
            return Err(Error(1));
        }
        let result = execute_native(self.inner.raw.as_ptr(), input);
        self.inner.active.store(false, Ordering::Release);
        result
    }
    pub fn execute_async(&self, input: String, spawner: Arc<dyn Spawner>) -> Result<RunFuture> {
        if self.inner.active.swap(true, Ordering::AcqRel) {
            return Err(Error(1));
        }
        let state = Arc::new(FutureState::default());
        let worker_state = state.clone();
        let run = self.inner.clone();
        if let Err(error) = spawner.spawn(Box::new(move || {
            let result = execute_native(run.raw.as_ptr(), &input);
            run.active.store(false, Ordering::Release);
            worker_state.finish(result);
        })) {
            self.inner.active.store(false, Ordering::Release);
            return Err(error);
        }
        Ok(RunFuture {
            state,
            run: self.inner.clone(),
            completed: false,
        })
    }
}

fn execute_native(run: RunRaw, input: &str) -> Result<String> {
    let mut required = 0usize;
    let first = unsafe { dage_run_execute(run, view(input), null_mut(), 0, &mut required) };
    if first != BUFFER_TOO_SMALL {
        return Err(Error(first));
    }
    let mut output = vec![0u8; required];
    check(unsafe {
        dage_run_execute(
            run,
            view(input),
            output.as_mut_ptr().cast(),
            output.len(),
            &mut required,
        )
    })?;
    if output.last() == Some(&0) {
        output.pop();
    }
    String::from_utf8(output).map_err(|_| Error(101))
}

pub trait Spawner: Send + Sync {
    fn spawn(&self, task: Box<dyn FnOnce() + Send>) -> Result<()>;
}
#[derive(Default)]
pub struct ThreadSpawner;
impl Spawner for ThreadSpawner {
    fn spawn(&self, task: Box<dyn FnOnce() + Send>) -> Result<()> {
        std::thread::Builder::new()
            .name("dage-rust".into())
            .spawn(task)
            .map(|_| ())
            .map_err(|_| Error(2))
    }
}

#[derive(Default)]
struct FutureSlot {
    value: Option<Result<String>>,
    waker: Option<Waker>,
}
#[derive(Default)]
struct FutureState(Mutex<FutureSlot>);
impl FutureState {
    fn finish(&self, value: Result<String>) {
        let waker = {
            let mut slot = self.0.lock().expect("future state poisoned");
            slot.value = Some(value);
            slot.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}
pub struct RunFuture {
    state: Arc<FutureState>,
    run: Arc<RunInner>,
    completed: bool,
}
impl Future for RunFuture {
    type Output = Result<String>;
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let value = {
            let mut slot = self.state.0.lock().expect("future state poisoned");
            if slot.value.is_none() {
                slot.waker = Some(cx.waker().clone());
                return Poll::Pending;
            }
            slot.value.take()
        };
        self.completed = true;
        Poll::Ready(value.expect("ready future lost its value"))
    }
}
impl Drop for RunFuture {
    fn drop(&mut self) {
        if !self.completed {
            let reason = "Rust Future dropped";
            unsafe {
                dage_run_cancel_with_reason(self.run.raw.as_ptr(), view(reason));
            }
        }
    }
}

pub type AsyncExecutorFuture = Pin<Box<dyn Future<Output = Result<String>> + Send + 'static>>;
struct Registration {
    callback: Box<dyn Fn(AsyncExecutionContext, String) -> AsyncExecutorFuture + Send + Sync>,
    spawner: Arc<dyn Spawner>,
}
struct Completion {
    handle: Mutex<Option<NonNull<c_void>>>,
}
unsafe impl Send for Completion {}
unsafe impl Sync for Completion {}
impl Completion {
    fn take(&self) -> Option<NonNull<c_void>> {
        self.handle.lock().expect("completion poisoned").take()
    }
    fn cancelled(&self) -> bool {
        let guard = self.handle.lock().expect("completion poisoned");
        match guard.as_ref() {
            None => true,
            Some(handle) => unsafe { dage_executor_completion_is_cancelled(handle.as_ptr()) != 0 },
        }
    }
    fn commit(&self) -> Result<()> {
        let guard = self.handle.lock().expect("completion poisoned");
        let handle = guard.as_ref().ok_or(Error(1))?;
        check(unsafe { dage_executor_completion_commit_effect(handle.as_ptr()) })
    }
    fn complete(&self, result: Result<String>) {
        let Some(handle) = self.take() else { return };
        unsafe {
            match result {
                Ok(json) => {
                    let output = OwnedBuffer {
                        data: json.as_ptr().cast(),
                        size: json.len(),
                        release: None,
                        release_userdata: null_mut(),
                    };
                    let _ = dage_executor_complete(handle.as_ptr(), OK, &output);
                }
                Err(_) => {
                    let _ =
                        dage_executor_complete(handle.as_ptr(), EXECUTION_ERROR, std::ptr::null());
                }
            }
        }
    }
    fn abandon(&self) {
        if let Some(handle) = self.take() {
            unsafe { dage_executor_abandon(handle.as_ptr()) }
        }
    }
}

#[derive(Clone)]
pub struct AsyncExecutionContext {
    completion: Arc<Completion>,
    pub run_id: String,
    pub node_id: String,
    pub node_type: String,
    pub attempt: u32,
    pub idempotency_key: String,
    pub run_mode: String,
    pub deadline_remaining_ms: u64,
}
impl AsyncExecutionContext {
    pub fn is_cancelled(&self) -> bool {
        self.completion.cancelled()
    }
    pub fn commit_effect(&self) -> Result<()> {
        self.completion.commit()
    }
}

unsafe extern "C" fn async_thunk(
    native: *const NativeExecutionContext,
    input: StringView,
    completion: CompletionRaw,
    userdata: *mut c_void,
) -> c_int {
    let started = catch_unwind(AssertUnwindSafe(|| {
        let registration = unsafe { &*userdata.cast::<Registration>() };
        let native = unsafe { &*native };
        let completion = Arc::new(Completion {
            handle: Mutex::new(NonNull::new(completion)),
        });
        let context = AsyncExecutionContext {
            completion: completion.clone(),
            run_id: unsafe { string_of(native.run_id) },
            node_id: unsafe { string_of(native.node_id) },
            node_type: unsafe { string_of(native.node_type) },
            attempt: native.attempt,
            idempotency_key: unsafe { string_of(native.idempotency_key) },
            run_mode: unsafe { string_of(native.run_mode) },
            deadline_remaining_ms: native.deadline_remaining_ms,
        };
        let input = unsafe { string_of(input) };
        let future = (registration.callback)(context, input);
        let task_completion = completion.clone();
        registration
            .spawner
            .spawn(Box::new(move || {
                let result = catch_unwind(AssertUnwindSafe(|| block_on(future)))
                    .unwrap_or(Err(Error(EXECUTION_ERROR)));
                if result == Err(Error(CANCELLED)) {
                    task_completion.abandon();
                } else {
                    task_completion.complete(result);
                }
            }))
            .map_err(|_| ())?;
        Ok::<(), ()>(())
    }));
    match started {
        Ok(Ok(())) => OK,
        _ => {
            // Returning failure transfers cleanup back to the C ABI.
            EXECUTION_ERROR
        }
    }
}
unsafe extern "C" fn destroy_registration(userdata: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
        drop(Box::from_raw(userdata.cast::<Registration>()));
    }));
}

struct Parker {
    ready: Mutex<bool>,
    signal: Condvar,
}
fn block_on(mut future: AsyncExecutorFuture) -> Result<String> {
    let parker = Arc::new(Parker {
        ready: Mutex::new(true),
        signal: Condvar::new(),
    });
    let waker = parker_waker(parker.clone());
    let mut context = Context::from_waker(&waker);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        let mut ready = parker.ready.lock().expect("executor parker poisoned");
        while !*ready {
            ready = parker.signal.wait(ready).expect("executor parker poisoned");
        }
        *ready = false;
    }
}
fn parker_waker(parker: Arc<Parker>) -> Waker {
    unsafe { Waker::from_raw(raw_waker(Arc::into_raw(parker).cast())) }
}
unsafe fn raw_waker(data: *const ()) -> RawWaker {
    RawWaker::new(
        data,
        &RawWakerVTable::new(clone_waker, wake, wake_by_ref, drop_waker),
    )
}
unsafe fn clone_waker(data: *const ()) -> RawWaker {
    let parker = unsafe { Arc::<Parker>::from_raw(data.cast()) };
    let cloned = parker.clone();
    std::mem::forget(parker);
    unsafe { raw_waker(Arc::into_raw(cloned).cast()) }
}
unsafe fn wake(data: *const ()) {
    let parker = unsafe { Arc::<Parker>::from_raw(data.cast()) };
    *parker.ready.lock().expect("executor parker poisoned") = true;
    parker.signal.notify_one();
}
unsafe fn wake_by_ref(data: *const ()) {
    let parker = unsafe { Arc::<Parker>::from_raw(data.cast()) };
    *parker.ready.lock().expect("executor parker poisoned") = true;
    parker.signal.notify_one();
    std::mem::forget(parker);
}
unsafe fn drop_waker(data: *const ()) {
    drop(unsafe { Arc::<Parker>::from_raw(data.cast()) });
}
