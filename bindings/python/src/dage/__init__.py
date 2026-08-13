"""Python binding for DAGE. Workflow semantics remain in the native C++ core."""
import asyncio
import ctypes
import json
import os
import sys
import threading
import weakref
from collections import deque
from dataclasses import dataclass
from enum import IntEnum


_DLL_DIRECTORY_HANDLES = []
__version__ = "0.2.0"
__all__ = [
    "AsyncExecutionContext", "DageError", "Engine", "EngineOptions", "Run",
    "RunMode", "RunOptions", "ScheduledTask", "TraceCapture", "Workflow",
    "__version__",
]


class RunMode(IntEnum):
    NORMAL = 0
    REPLAY = 1
    SHADOW = 2


class TraceCapture(IntEnum):
    METADATA = 0
    OFF = 1
    INPUTS = 2
    FULL = 3


@dataclass(frozen=True)
class EngineOptions:
    max_workflow_bytes: int = 0
    max_json_depth: int = 0
    max_nodes: int = 0
    max_edges: int = 0
    max_expression_bytes: int = 0
    max_literal_bytes: int = 0
    max_compiled_ir_bytes: int = 0


@dataclass(frozen=True)
class RunOptions:
    mode: RunMode = RunMode.NORMAL
    allow_external_writes: bool = False
    allow_irreversible: bool = False
    trace_capture: TraceCapture = TraceCapture.METADATA
    deadline_ms: int = 0
    retry_budget: int = 0
    max_output_bytes: int = 0
    max_state_bytes: int = 0
    max_events: int = 0
    max_in_flight_tasks: int = 0


def _prepare_dll_search(library):
    if sys.platform != "win32" or not hasattr(os, "add_dll_directory"):
        return

    library_directory = os.path.dirname(os.path.abspath(library))
    candidates = [library_directory]
    candidates.extend(os.environ.get("PATH", "").split(os.pathsep))
    runtime_files = (
        "libstdc++-6.dll",
        "libgcc_s_seh-1.dll",
        "libwinpthread-1.dll",
        "libcrypto-3-x64.dll",
    )
    for directory in candidates:
        if not directory or not os.path.isdir(directory):
            continue
        if directory != library_directory and not all(
            os.path.exists(os.path.join(directory, filename))
            for filename in runtime_files
        ):
            continue
        try:
            _DLL_DIRECTORY_HANDLES.append(os.add_dll_directory(directory))
        except OSError:
            pass


class DageError(RuntimeError):
    def __init__(self, status, message=None):
        self.status = status
        self.message = message
        detail = f": {message}" if message else ""
        super().__init__(f"DAGE status {status}{detail}")


def _default_library():
    package_native = os.path.join(os.path.dirname(__file__), "_native")
    root = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "..", ".."))
    names = (
        ["libdage.dll", "dage.dll"]
        if sys.platform == "win32"
        else ["libdage.dylib", "libdage.so"]
    )
    if os.path.isdir(package_native):
        for name in names:
            path = os.path.join(package_native, name)
            if os.path.isfile(path):
                return path
        raise DageError(7, "installed DAGE wheel has no bundled native runtime")
    directories = (os.path.join(root, "build"), os.path.join(root, "build-shared"), root)
    for name in names:
        for directory in directories:
            path = os.path.join(directory, name)
            if os.path.exists(path):
                return path
    raise DageError(7, "DAGE shared library not found; set DAGE_LIBRARY")


class _View(ctypes.Structure):
    _fields_ = [("data", ctypes.c_char_p), ("size", ctypes.c_size_t)]

_CommitEffect = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
_IsCancelled = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p)
_ReleaseBuffer = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p)

class _OwnedBuffer(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.c_char_p),
        ("size", ctypes.c_size_t),
        ("release", _ReleaseBuffer),
        ("release_userdata", ctypes.c_void_p),
    ]

class _ExecutionContext(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("run_id", _View),
        ("node_id", _View),
        ("node_type", _View),
        ("attempt", ctypes.c_uint32),
        ("idempotency_key", _View),
        ("run_mode", _View),
        ("deadline_remaining_ms", ctypes.c_uint64),
        ("commit_effect", _CommitEffect),
        ("commit_effect_userdata", ctypes.c_void_p),
        ("is_cancelled", _IsCancelled),
        ("cancellation_userdata", ctypes.c_void_p),
    ]


class _EngineOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("output_allocator", ctypes.c_void_p),
        ("max_workflow_bytes", ctypes.c_uint64),
        ("max_json_depth", ctypes.c_uint32),
        ("max_nodes", ctypes.c_uint32),
        ("max_edges", ctypes.c_uint64),
        ("max_expression_bytes", ctypes.c_uint64),
        ("max_literal_bytes", ctypes.c_uint64),
        ("max_compiled_ir_bytes", ctypes.c_uint64),
        ("reserved", ctypes.c_void_p * 2),
    ]


class _RunOptions(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("mode", ctypes.c_int),
        ("allow_external_writes", ctypes.c_uint8),
        ("allow_irreversible", ctypes.c_uint8),
        ("trace_capture", ctypes.c_uint8),
        ("reserved_bytes", ctypes.c_uint8 * 5),
        ("deadline_ms", ctypes.c_uint64),
        ("retry_budget", ctypes.c_uint32),
        ("reserved_u32", ctypes.c_uint32),
        ("max_output_bytes", ctypes.c_uint64),
        ("max_state_bytes", ctypes.c_uint64),
        ("max_events", ctypes.c_uint64),
        ("max_in_flight_tasks", ctypes.c_uint32),
        ("reserved_quota", ctypes.c_uint32),
        ("reserved", ctypes.c_void_p * 4),
    ]


_SchedulerSubmit = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_uint8, ctypes.c_void_p)
_TraceEmit = ctypes.CFUNCTYPE(None, _View, ctypes.c_void_p)
_StatePut = ctypes.CFUNCTYPE(ctypes.c_int, _View, _View, ctypes.c_void_p)
_StateGet = ctypes.CFUNCTYPE(ctypes.c_int, _View, ctypes.POINTER(_OwnedBuffer), ctypes.c_void_p)
_StateErase = ctypes.CFUNCTYPE(ctypes.c_int, _View, ctypes.c_void_p)


class _StateRecord(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32), ("version", ctypes.c_uint64),
        ("epoch", ctypes.c_uint64), ("owner", _OwnedBuffer),
        ("checkpoint", _OwnedBuffer), ("reserved", ctypes.c_void_p * 4),
    ]


_StateLoad = ctypes.CFUNCTYPE(ctypes.c_int, _View, ctypes.POINTER(_StateRecord), ctypes.c_void_p)
_StateCas = ctypes.CFUNCTYPE(
    ctypes.c_int, _View, ctypes.c_uint64, _View,
    ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p)
_StateClaim = ctypes.CFUNCTYPE(
    ctypes.c_int, _View, ctypes.c_uint64, _View,
    ctypes.POINTER(_StateRecord), ctypes.c_void_p)
_StateList = ctypes.CFUNCTYPE(ctypes.c_int, _View, ctypes.POINTER(_OwnedBuffer), ctypes.c_void_p)
_LeaseRenew = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p)
_LeaseRelease = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class _ResourceLease(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32), ("lease_id", _OwnedBuffer),
        ("fencing_token", ctypes.c_uint64), ("expires_at_unix_ms", ctypes.c_uint64),
        ("renew", _LeaseRenew), ("release", _LeaseRelease),
        ("lease_userdata", ctypes.c_void_p), ("reserved", ctypes.c_void_p * 4),
    ]


_LeaseAcquire = ctypes.CFUNCTYPE(
    ctypes.c_int, _View, _IsCancelled, ctypes.c_void_p,
    ctypes.POINTER(_ResourceLease), ctypes.c_void_p)


class _SchedulerVtable(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("submit", _SchedulerSubmit),
                ("reserved", ctypes.c_void_p * 4)]


class _TraceSinkVtable(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("emit", _TraceEmit),
                ("reserved", ctypes.c_void_p * 4)]


class _StateStoreVtable(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32), ("put", _StatePut), ("get", _StateGet),
        ("erase", _StateErase), ("load", _StateLoad), ("compare_exchange", _StateCas),
        ("claim", _StateClaim), ("list", _StateList), ("reserved", ctypes.c_void_p * 4),
    ]


class _ResourceLeaseProviderVtable(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("acquire", _LeaseAcquire),
                ("reserved", ctypes.c_void_p * 4)]


class _OwnedBufferPool:
    def __init__(self):
        self._lock = threading.Lock()
        self._buffers = {}

        @_ReleaseBuffer
        def release(_data, _size, token):
            with self._lock:
                self._buffers.pop(int(token or 0), None)

        self.release = release

    def make(self, value):
        data = value if isinstance(value, bytes) else str(value).encode("utf-8")
        buffer = ctypes.create_string_buffer(data)
        token = ctypes.addressof(buffer)
        with self._lock:
            self._buffers[token] = buffer
        return _OwnedBuffer(ctypes.cast(buffer, ctypes.c_char_p), len(data),
                            self.release, ctypes.c_void_p(token))


class ScheduledTask:
    """One-shot native scheduler task transferred to a Python Scheduler."""

    def __init__(self, library, handle):
        self._library = library
        self._handle = handle
        self._lock = threading.Lock()

    def _take(self):
        with self._lock:
            handle, self._handle = self._handle, None
        return handle

    def run(self):
        handle = self._take()
        if not handle:
            raise RuntimeError("scheduler task has already been consumed")
        status = self._library.dage_scheduler_task_run(handle)
        if status:
            raise DageError(status)

    def abandon(self):
        handle = self._take()
        if handle:
            self._library.dage_scheduler_task_abandon(handle)

    def __del__(self):
        self.abandon()

_AsyncExecutor = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.POINTER(_ExecutionContext), _View, ctypes.c_void_p, ctypes.c_void_p)


def _view(text):
    data = text.encode("utf-8")
    return data, _View(data, len(data))


def _decode_view(view):
    return ctypes.string_at(view.data, view.size).decode("utf-8")


class AsyncExecutionContext:
    """Copied async Executor context. Methods are valid until the Executor completes."""

    def __init__(self, library, native, completion):
        self.run_id = _decode_view(native.run_id)
        self.node_id = _decode_view(native.node_id)
        self.node_type = _decode_view(native.node_type)
        self.attempt = native.attempt
        self.idempotency_key = _decode_view(native.idempotency_key)
        self.run_mode = _decode_view(native.run_mode)
        self.deadline_remaining_ms = native.deadline_remaining_ms
        self._library = library
        self._completion = completion

    def is_cancelled(self):
        return self._completion.is_cancelled()

    def commit_effect(self):
        self._completion.commit_effect()


class _Completion:
    """Python-side one-shot guard for a transferred native completion handle."""

    def __init__(self, library, handle, on_finished):
        self._library = library
        self._handle = handle
        self._lock = threading.Lock()
        self._on_finished = on_finished

    def _take(self):
        with self._lock:
            handle, self._handle = self._handle, None
        if handle:
            self._on_finished(self)
        return handle

    def is_cancelled(self):
        with self._lock:
            handle = self._handle
            return True if not handle else bool(
                self._library.dage_executor_completion_is_cancelled(handle)
            )

    def commit_effect(self):
        with self._lock:
            handle = self._handle
            if not handle:
                raise DageError(1)
            status = self._library.dage_executor_completion_commit_effect(handle)
        if status:
            raise DageError(status)

    def complete(self, status, value=None):
        handle = self._take()
        if not handle:
            return
        if status == 0:
            encoded = json.dumps(value, separators=(",", ":")).encode("utf-8")
            buffer = ctypes.create_string_buffer(encoded)
            output = _OwnedBuffer(
                ctypes.cast(buffer, ctypes.c_char_p), len(encoded), _ReleaseBuffer(), None
            )
            result = self._library.dage_executor_complete(handle, status, ctypes.byref(output))
        else:
            result = self._library.dage_executor_complete(handle, status, None)
        if result:
            raise DageError(result)

    def abandon(self):
        handle = self._take()
        if handle:
            self._library.dage_executor_abandon(handle)


class Engine:
    """Owns a native engine. Keep it alive longer than workflows and runs."""

    def __init__(self, library=None, options=None):
        library = library or os.environ.get("DAGE_LIBRARY") or _default_library()
        _prepare_dll_search(library)
        self.lib = ctypes.CDLL(library)
        self.lib.dage_engine_create.argtypes = [
            ctypes.POINTER(_EngineOptions), ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_engine_create.restype = ctypes.c_int
        self.lib.dage_engine_load.argtypes = [
            ctypes.c_void_p, _View, ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_engine_load.restype = ctypes.c_int
        self.lib.dage_workflow_apply_patch.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, _View, ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_workflow_apply_patch.restype = ctypes.c_int
        for name in ("dage_workflow_dry_run_patch", "dage_workflow_analyze_patch"):
            function = getattr(self.lib, name)
            function.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, _View, ctypes.c_char_p,
                ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
            function.restype = ctypes.c_int
        self.lib.dage_workflow_diff.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
            ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.lib.dage_workflow_diff.restype = ctypes.c_int
        self.lib.dage_workflow_destroy.argtypes = [ctypes.c_void_p]
        self.lib.dage_run_create.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                             ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_run_create.restype = ctypes.c_int
        self.lib.dage_run_destroy.argtypes = [ctypes.c_void_p]
        self.lib.dage_run_snapshot.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t)]
        self.lib.dage_run_snapshot.restype = ctypes.c_int
        self.lib.dage_run_selective_rerun.argtypes = [
            ctypes.c_void_p, _View, ctypes.c_char_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t)]
        self.lib.dage_run_selective_rerun.restype = ctypes.c_int
        self._callback_type = ctypes.CFUNCTYPE(
            ctypes.c_int, ctypes.POINTER(_ExecutionContext), _View,
            ctypes.POINTER(_OwnedBuffer), ctypes.c_void_p)
        self.lib.dage_engine_register_executor.argtypes = [
            ctypes.c_void_p, _View, self._callback_type, ctypes.c_void_p, ctypes.c_void_p]
        self.lib.dage_engine_register_async_executor.argtypes = [
            ctypes.c_void_p, _View, _AsyncExecutor, ctypes.c_void_p, ctypes.c_void_p]
        self.lib.dage_engine_register_async_executor.restype = ctypes.c_int
        self.lib.dage_executor_complete.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(_OwnedBuffer)]
        self.lib.dage_executor_complete.restype = ctypes.c_int
        self.lib.dage_executor_abandon.argtypes = [ctypes.c_void_p]
        self.lib.dage_executor_completion_is_cancelled.argtypes = [ctypes.c_void_p]
        self.lib.dage_executor_completion_is_cancelled.restype = ctypes.c_uint8
        self.lib.dage_executor_completion_commit_effect.argtypes = [ctypes.c_void_p]
        self.lib.dage_executor_completion_commit_effect.restype = ctypes.c_int
        self._callbacks = []
        self._host_adapters = []
        self._callback_errors = deque(maxlen=256)
        self._workflows = weakref.WeakSet()
        self._runs = weakref.WeakSet()
        self._async_lock = threading.Lock()
        self._async_completions = set()
        self._async_futures = set()
        self.lib.dage_engine_destroy.argtypes = [ctypes.c_void_p]
        expected_sizes = {
            _EngineOptions: 80, _RunOptions: 96, _ExecutionContext: 136,
            _OwnedBuffer: 32, _SchedulerVtable: 48, _StateRecord: 120,
            _StateStoreVtable: 96, _TraceSinkVtable: 48,
            _ResourceLease: 112, _ResourceLeaseProviderVtable: 48,
        }
        if ctypes.sizeof(ctypes.c_void_p) != 8 or any(
                ctypes.sizeof(structure) != size
                for structure, size in expected_sizes.items()):
            raise DageError(1, "Python binding requires the frozen 64-bit DAGE C ABI layout")
        self.handle = ctypes.c_void_p()
        native_options = None
        if options is not None:
            if not isinstance(options, EngineOptions):
                raise TypeError("options must be an EngineOptions instance")
            native_options = _EngineOptions()
            native_options.struct_size = ctypes.sizeof(_EngineOptions)
            native_options.api_version = 1
            for field in (
                "max_workflow_bytes", "max_json_depth", "max_nodes", "max_edges",
                "max_expression_bytes", "max_literal_bytes", "max_compiled_ir_bytes",
            ):
                value = getattr(options, field)
                if value < 0:
                    raise ValueError(f"{field} must be non-negative")
                setattr(native_options, field, value)
        self._check(self.lib.dage_engine_create(
            ctypes.byref(native_options) if native_options is not None else None,
            ctypes.byref(self.handle),
        ))
        registry = self.runtime_registry()
        runtime_version = str(registry.get("runtime_version", ""))
        if registry.get("c_abi_version") != 1 or runtime_version.split(".")[:2] != ["0", "2"]:
            self.lib.dage_engine_destroy(self.handle)
            self.handle = None
            raise DageError(
                1, f"incompatible DAGE runtime {runtime_version or 'unknown'}; "
                f"Python package {__version__} requires C ABI 1 and runtime 0.2.x",
            )

    def _json_output(self, function, *arguments):
        required = ctypes.c_size_t()
        status = function(*arguments, None, 0, ctypes.byref(required))
        if status != 8:
            self._check(status)
        output = ctypes.create_string_buffer(required.value)
        self._check(function(*arguments, output, len(output), ctypes.byref(required)))
        return json.loads(output.value.decode("utf-8"))

    def has_capability(self, capability):
        keepalive, view = _view(capability)
        self.lib.dage_runtime_has_capability.argtypes = [_View]
        self.lib.dage_runtime_has_capability.restype = ctypes.c_uint8
        return bool(self.lib.dage_runtime_has_capability(view))

    def runtime_registry(self):
        self.lib.dage_runtime_registry.argtypes = [
            ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        return self._json_output(self.lib.dage_runtime_registry)

    def set_scheduler(self, submit):
        """Install ``submit(task, continuation)``; it must eventually run or abandon task."""
        self.lib.dage_scheduler_task_run.argtypes = [ctypes.c_void_p]
        self.lib.dage_scheduler_task_run.restype = ctypes.c_int
        self.lib.dage_scheduler_task_abandon.argtypes = [ctypes.c_void_p]

        @_SchedulerSubmit
        def callback(handle, continuation, _userdata):
            task = ScheduledTask(self.lib, handle)
            try:
                submit(task, bool(continuation))
                return 0
            except BaseException as error:
                self._callback_failed("scheduler", error)
                task.abandon()
                return 100

        vtable = _SchedulerVtable()
        vtable.struct_size = ctypes.sizeof(vtable)
        vtable.submit = callback
        self.lib.dage_engine_set_scheduler.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(_SchedulerVtable), ctypes.c_void_p, ctypes.c_void_p]
        self.lib.dage_engine_set_scheduler.restype = ctypes.c_int
        self._check(self.lib.dage_engine_set_scheduler(
            self.handle, ctypes.byref(vtable), None, None))
        self._host_adapters.append((callback, vtable))

    def set_trace_sink(self, emit):
        """Install a non-throwing adapter that receives decoded structured trace events."""
        @_TraceEmit
        def callback(event, _userdata):
            try:
                emit(json.loads(_decode_view(event)))
            except BaseException as error:
                self._callback_failed("trace_sink", error)
                # The C trace callback cannot report failure. Never cross the ABI boundary.
                return

        vtable = _TraceSinkVtable()
        vtable.struct_size = ctypes.sizeof(vtable)
        vtable.emit = callback
        self.lib.dage_engine_set_trace_sink.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(_TraceSinkVtable), ctypes.c_void_p, ctypes.c_void_p]
        self.lib.dage_engine_set_trace_sink.restype = ctypes.c_int
        self._check(self.lib.dage_engine_set_trace_sink(
            self.handle, ctypes.byref(vtable), None, None))
        self._host_adapters.append((callback, vtable))

    def set_state_store(self, store):
        """Install a thread-safe StateStore implementing put/get/erase/load/CAS/claim/list."""
        buffers = _OwnedBufferPool()

        def fill_record(output, record):
            output.contents.struct_size = ctypes.sizeof(_StateRecord)
            output.contents.version = int(record["version"])
            output.contents.epoch = int(record.get("epoch", 0))
            output.contents.owner = buffers.make(record.get("owner", ""))
            output.contents.checkpoint = buffers.make(record["checkpoint"])

        @_StatePut
        def put(run_id, checkpoint, _):
            try:
                store.put(_decode_view(run_id), _decode_view(checkpoint)); return 0
            except BaseException as error:
                self._callback_failed("state_store.put", error); return 100

        @_StateGet
        def get(run_id, output, _):
            try:
                value = store.get(_decode_view(run_id))
                if value is None: return 7
                output.contents = buffers.make(value); return 0
            except BaseException as error:
                self._callback_failed("state_store.get", error); return 100

        @_StateErase
        def erase(run_id, _):
            try:
                store.erase(_decode_view(run_id)); return 0
            except BaseException as error:
                self._callback_failed("state_store.erase", error); return 100

        @_StateLoad
        def load(run_id, output, _):
            try:
                value = store.load(_decode_view(run_id))
                if value is None: return 7
                fill_record(output, value); return 0
            except BaseException as error:
                self._callback_failed("state_store.load", error); return 100

        @_StateCas
        def compare_exchange(run_id, expected, checkpoint, version, _):
            try:
                value = store.compare_exchange(
                    _decode_view(run_id), int(expected), _decode_view(checkpoint))
                if value is None: return 5
                version.contents.value = int(value); return 0
            except BaseException as error:
                self._callback_failed("state_store.compare_exchange", error); return 100

        @_StateClaim
        def claim(run_id, expected, owner, output, _):
            try:
                value = store.claim(_decode_view(run_id), int(expected), _decode_view(owner))
                if value is None: return 5
                fill_record(output, value); return 0
            except BaseException as error:
                self._callback_failed("state_store.claim", error); return 100

        @_StateList
        def list_runs(prefix, output, _):
            try:
                output.contents = buffers.make(json.dumps(
                    list(store.list(_decode_view(prefix))), separators=(",", ":")))
                return 0
            except BaseException as error:
                self._callback_failed("state_store.list", error); return 100

        vtable = _StateStoreVtable()
        vtable.struct_size = ctypes.sizeof(vtable)
        vtable.put, vtable.get, vtable.erase, vtable.load = put, get, erase, load
        vtable.compare_exchange, vtable.claim, vtable.list = compare_exchange, claim, list_runs
        self.lib.dage_engine_set_state_store.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(_StateStoreVtable), ctypes.c_void_p, ctypes.c_void_p]
        self.lib.dage_engine_set_state_store.restype = ctypes.c_int
        self._check(self.lib.dage_engine_set_state_store(
            self.handle, ctypes.byref(vtable), None, None))
        self._host_adapters.append((buffers, vtable, put, get, erase, load,
                                    compare_exchange, claim, list_runs))

    def set_resource_lease_provider(self, provider):
        """Install a provider whose acquire(request, cancelled) returns a lease object."""
        buffers = _OwnedBufferPool()
        active = {}
        active_lock = threading.Lock()

        @_LeaseRenew
        def renew(ttl_ms, expires, token):
            with active_lock:
                lease = active.get(int(token or 0))
            if lease is None:
                return 5
            try:
                expires.contents.value = int(lease.renew(int(ttl_ms)))
                return 0
            except BaseException as error:
                self._callback_failed("resource_lease.renew", error)
                return 100

        @_LeaseRelease
        def release(token):
            with active_lock:
                lease = active.pop(int(token or 0), None)
            if lease is not None:
                try:
                    lease.release()
                except BaseException as error:
                    self._callback_failed("resource_lease.release", error)
                    return

        @_LeaseAcquire
        def acquire(request, is_cancelled, cancellation, output, _):
            try:
                lease = provider.acquire(
                    json.loads(_decode_view(request)),
                    lambda: bool(is_cancelled(cancellation)),
                )
                if lease is None:
                    return 6 if is_cancelled(cancellation) else 5
                token = id(lease)
                with active_lock:
                    active[token] = lease
                output.contents.struct_size = ctypes.sizeof(_ResourceLease)
                output.contents.lease_id = buffers.make(lease.lease_id)
                output.contents.fencing_token = int(lease.fencing_token)
                output.contents.expires_at_unix_ms = int(lease.expires_at_unix_ms)
                output.contents.renew = renew
                output.contents.release = release
                output.contents.lease_userdata = ctypes.c_void_p(token)
                return 0
            except BaseException as error:
                self._callback_failed("resource_lease.acquire", error)
                return 100

        vtable = _ResourceLeaseProviderVtable()
        vtable.struct_size = ctypes.sizeof(vtable)
        vtable.acquire = acquire
        self.lib.dage_engine_set_resource_lease_provider.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(_ResourceLeaseProviderVtable),
            ctypes.c_void_p, ctypes.c_void_p]
        self.lib.dage_engine_set_resource_lease_provider.restype = ctypes.c_int
        self._check(self.lib.dage_engine_set_resource_lease_provider(
            self.handle, ctypes.byref(vtable), None, None))
        self._host_adapters.append((buffers, active, vtable, acquire, renew, release))

    def _check(self, status):
        if status:
            message = None
            if getattr(self, "handle", None):
                try:
                    self.lib.dage_last_error.argtypes = [
                        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                        ctypes.POINTER(ctypes.c_size_t)]
                    required = ctypes.c_size_t()
                    first = self.lib.dage_last_error(
                        self.handle, None, 0, ctypes.byref(required))
                    if first == 8 and required.value:
                        output = ctypes.create_string_buffer(required.value)
                        if self.lib.dage_last_error(
                                self.handle, output, len(output), ctypes.byref(required)) == 0:
                            message = output.value.decode("utf-8")
                except (AttributeError, UnicodeError, OSError):
                    pass
            raise DageError(status, message)

    def _callback_failed(self, source, error):
        with self._async_lock:
            self._callback_errors.append((source, error))

    def callback_errors(self):
        """Return and clear exceptions contained at native callback boundaries."""
        with self._async_lock:
            errors = list(self._callback_errors)
            self._callback_errors.clear()
        return errors

    def load(self, document):
        text = document if isinstance(document, str) else json.dumps(document)
        keepalive, view = _view(text)
        handle = ctypes.c_void_p()
        self._check(self.lib.dage_engine_load(self.handle, view, ctypes.byref(handle)))
        workflow = Workflow(self, handle)
        self._workflows.add(workflow)
        return workflow

    def restore(self, workflow, checkpoint):
        if workflow.engine is not self or not workflow.handle:
            raise ValueError("workflow must be a live Workflow owned by this Engine")
        text = checkpoint if isinstance(checkpoint, str) else json.dumps(checkpoint)
        keepalive, view = _view(text)
        handle = ctypes.c_void_p()
        self.lib.dage_run_restore.argtypes = [ctypes.c_void_p, ctypes.c_void_p, _View,
                                              ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_run_restore.restype = ctypes.c_int
        self._check(self.lib.dage_run_restore(
            self.handle, workflow.handle, view, ctypes.byref(handle)))
        run = Run(self, handle, workflow)
        self._runs.add(run)
        return run

    def register_executor(self, name, function):
        """Register callable(node_id, decoded_input) -> JSON-compatible value.

        An external-write callable may return ``(value, True)`` to confirm its
        durable side effect and close the runtime commit fence.
        """
        buffers = {}

        @_ReleaseBuffer
        def release_buffer(_data, _size, token):
            buffers.pop(int(token or 0), None)

        @self._callback_type
        def callback(context, input_value, output, _):
            try:
                node = context.contents.node_id
                value = function(
                    ctypes.string_at(node.data, node.size).decode("utf-8"),
                    json.loads(ctypes.string_at(
                        input_value.data, input_value.size).decode("utf-8")))
                if isinstance(value, tuple) and len(value) == 2:
                    value, committed = value
                    if committed and context.contents.commit_effect:
                        context.contents.commit_effect(context.contents.commit_effect_userdata)
                encoded = json.dumps(value, separators=(",", ":")).encode("utf-8")
                buffer = ctypes.create_string_buffer(encoded)
                token = ctypes.addressof(buffer)
                buffers[token] = buffer
                output.contents.data = ctypes.cast(buffer, ctypes.c_char_p)
                output.contents.size = len(encoded)
                output.contents.release = release_buffer
                output.contents.release_userdata = ctypes.c_void_p(token)
                return 0
            except BaseException as error:
                self._callback_failed(f"executor:{name}", error)
                return 5

        keepalive, view = _view(name)
        self._check(self.lib.dage_engine_register_executor(
            self.handle, view, callback, None, None))
        self._callbacks.extend((callback, release_buffer))

    def register_async_executor(self, name, function, loop=None):
        """Register ``async function(context, decoded_input)`` on an asyncio loop."""
        event_loop = loop or asyncio.get_running_loop()

        def completion_finished(completion):
            with self._async_lock:
                self._async_completions.discard(completion)

        @_AsyncExecutor
        def callback(context, input_value, native_completion, _):
            completion = _Completion(self.lib, native_completion, completion_finished)
            with self._async_lock:
                self._async_completions.add(completion)
            try:
                copied_context = AsyncExecutionContext(
                    self.lib, context.contents, completion
                )
                decoded_input = json.loads(_decode_view(input_value))

                async def invoke():
                    try:
                        value = await function(copied_context, decoded_input)
                        committed = False
                        if isinstance(value, tuple) and len(value) == 2:
                            value, committed = value
                        if committed:
                            copied_context.commit_effect()
                        completion.complete(0, value)
                    except asyncio.CancelledError:
                        completion.abandon()
                        raise
                    except BaseException as error:
                        self._callback_failed(f"async_executor:{name}", error)
                        completion.complete(5)

                future = asyncio.run_coroutine_threadsafe(invoke(), event_loop)
                with self._async_lock:
                    self._async_futures.add(future)
                future.add_done_callback(self._discard_async_future)
                return 0
            except BaseException as error:
                self._callback_failed(f"async_executor_dispatch:{name}", error)
                completion.abandon()
                return 0

        keepalive, view = _view(name)
        self._check(self.lib.dage_engine_register_async_executor(
            self.handle, view, callback, None, None))
        self._callbacks.append(callback)

    def _discard_async_future(self, future):
        with self._async_lock:
            self._async_futures.discard(future)

    def close(self):
        if self.handle:
            if any(run.handle for run in self._runs) or any(
                    workflow.handle for workflow in self._workflows):
                raise RuntimeError(
                    "close all Runs and Workflows before closing their Engine")
            with self._async_lock:
                completions = list(self._async_completions)
                futures = list(self._async_futures)
            for completion in completions:
                completion.abandon()
            for future in futures:
                future.cancel()
            self.lib.dage_engine_destroy(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Workflow:
    def __init__(self, engine, handle):
        self.engine, self.handle = engine, handle

    def _text(self, function):
        function.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                             ctypes.POINTER(ctypes.c_size_t)]
        required = ctypes.c_size_t()
        status = function(self.handle, None, 0, ctypes.byref(required))
        if status != 8:
            self.engine._check(status)
        output = ctypes.create_string_buffer(required.value)
        self.engine._check(function(self.handle, output, len(output), ctypes.byref(required)))
        return output.value.decode("utf-8")

    def format(self):
        return self._text(self.engine.lib.dage_workflow_format)

    def mermaid(self):
        return self._text(self.engine.lib.dage_workflow_export_mermaid)

    def dot(self):
        return self._text(self.engine.lib.dage_workflow_export_dot)

    def create_run(self, options=None):
        handle = ctypes.c_void_p()
        if options is None:
            self.engine._check(self.engine.lib.dage_run_create(
                self.engine.handle, self.handle, ctypes.byref(handle)))
        else:
            if not isinstance(options, RunOptions):
                raise TypeError("options must be a RunOptions instance")
            native = _RunOptions()
            native.struct_size = ctypes.sizeof(_RunOptions)
            native.mode = int(options.mode)
            native.allow_external_writes = options.allow_external_writes
            native.allow_irreversible = options.allow_irreversible
            native.trace_capture = int(options.trace_capture)
            for field in ("deadline_ms", "retry_budget", "max_output_bytes", "max_state_bytes",
                          "max_events", "max_in_flight_tasks"):
                value = getattr(options, field)
                if value < 0:
                    raise ValueError(f"{field} must be non-negative")
                setattr(native, field, value)
            self.engine.lib.dage_run_create_with_options.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(_RunOptions),
                ctypes.POINTER(ctypes.c_void_p)]
            self.engine.lib.dage_run_create_with_options.restype = ctypes.c_int
            self.engine._check(self.engine.lib.dage_run_create_with_options(
                self.engine.handle, self.handle, ctypes.byref(native), ctypes.byref(handle)))
        run = Run(self.engine, handle, self)
        self.engine._runs.add(run)
        return run

    def _patch_text(self, function, patch):
        text = patch if isinstance(patch, str) else json.dumps(patch)
        keepalive, view = _view(text)
        return self.engine._json_output(function, self.engine.handle, self.handle, view)

    def dry_run_patch(self, patch):
        return self._patch_text(self.engine.lib.dage_workflow_dry_run_patch, patch)

    def analyze_patch(self, patch):
        return self._patch_text(self.engine.lib.dage_workflow_analyze_patch, patch)

    def apply_patch(self, patch):
        text = patch if isinstance(patch, str) else json.dumps(patch)
        keepalive, view = _view(text)
        handle = ctypes.c_void_p()
        self.engine._check(self.engine.lib.dage_workflow_apply_patch(
            self.engine.handle, self.handle, view, ctypes.byref(handle)))
        workflow = Workflow(self.engine, handle)
        self.engine._workflows.add(workflow)
        return workflow

    def diff(self, other):
        if not isinstance(other, Workflow) or other.engine is not self.engine:
            raise ValueError("other must be a Workflow owned by the same Engine")
        return self.engine._json_output(
            self.engine.lib.dage_workflow_diff,
            self.engine.handle, self.handle, other.handle,
        )

    def close(self):
        if self.handle:
            if any(run.handle and run.workflow is self for run in self.engine._runs):
                raise RuntimeError("close all Runs before closing their Workflow")
            self.engine.lib.dage_workflow_destroy(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Run:
    def __init__(self, engine, handle, workflow):
        self.engine, self.workflow, self.handle = engine, workflow, handle
        self._operation_lock = threading.Lock()

    def _operation(self, function, value=None):
        if not self._operation_lock.acquire(blocking=False):
            raise RuntimeError("a Run permits only one active operation")
        try:
            if not self.handle:
                raise RuntimeError("Run is closed")
            function.argtypes = [ctypes.c_void_p, _View, ctypes.c_char_p,
                                 ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
            actual_value = {} if value is None else value
            encoded, view = _view(json.dumps(actual_value, separators=(",", ":")))
            required = ctypes.c_size_t()
            status = function(self.handle, view, None, 0, ctypes.byref(required))
            if status == 9:
                return None
            if status != 8:
                self.engine._check(status)
            output = ctypes.create_string_buffer(required.value)
            self.engine._check(function(self.handle, view, output, len(output),
                                        ctypes.byref(required)))
            return json.loads(output.value.decode("utf-8"))
        finally:
            self._operation_lock.release()

    def execute(self, value=None):
        return self._operation(self.engine.lib.dage_run_execute, value)

    async def execute_async(self, value=None):
        """Execute without blocking the event loop and propagate task cancellation."""
        task = asyncio.create_task(asyncio.to_thread(self.execute, value))
        try:
            return await asyncio.shield(task)
        except asyncio.CancelledError:
            self.cancel("python asyncio task cancelled")
            try:
                await asyncio.shield(task)
            except (DageError, asyncio.CancelledError):
                pass
            raise

    def cancel(self, reason="cancelled by Python host"):
        self.engine.lib.dage_run_cancel_with_reason.argtypes = [ctypes.c_void_p, _View]
        self.engine.lib.dage_run_cancel_with_reason.restype = ctypes.c_int
        keepalive, view = _view(reason)
        self.engine._check(self.engine.lib.dage_run_cancel_with_reason(self.handle, view))

    def resume(self, human_output):
        return self._operation(self.engine.lib.dage_run_resume, human_output)

    async def resume_async(self, human_output):
        """Resume without blocking the event loop and propagate task cancellation."""
        task = asyncio.create_task(asyncio.to_thread(self.resume, human_output))
        try:
            return await asyncio.shield(task)
        except asyncio.CancelledError:
            self.cancel("python asyncio resume task cancelled")
            try:
                await asyncio.shield(task)
            except (DageError, asyncio.CancelledError):
                pass
            raise

    def checkpoint(self):
        if not self._operation_lock.acquire(blocking=False):
            raise RuntimeError("a Run permits only one active operation")
        try:
            function = self.engine.lib.dage_run_checkpoint
            function.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
                                 ctypes.POINTER(ctypes.c_size_t)]
            required = ctypes.c_size_t()
            status = function(self.handle, None, 0, ctypes.byref(required))
            if status != 8:
                self.engine._check(status)
            output = ctypes.create_string_buffer(required.value)
            self.engine._check(function(self.handle, output, len(output),
                                        ctypes.byref(required)))
            return output.value.decode("utf-8")
        finally:
            self._operation_lock.release()

    def snapshot(self):
        return self.engine._json_output(self.engine.lib.dage_run_snapshot, self.handle)

    def selective_rerun(self, node_id):
        if not self._operation_lock.acquire(blocking=False):
            raise RuntimeError("a Run permits only one active operation")
        try:
            keepalive, view = _view(node_id)
            return self.engine._json_output(
                self.engine.lib.dage_run_selective_rerun, self.handle, view)
        finally:
            self._operation_lock.release()

    async def selective_rerun_async(self, node_id):
        """Selectively rerun a node without blocking the event loop."""
        task = asyncio.create_task(asyncio.to_thread(self.selective_rerun, node_id))
        try:
            return await asyncio.shield(task)
        except asyncio.CancelledError:
            self.cancel("python asyncio selective rerun task cancelled")
            try:
                await asyncio.shield(task)
            except (DageError, asyncio.CancelledError):
                pass
            raise

    def close(self):
        if self.handle:
            self.cancel("Python Run close requested")
            with self._operation_lock:
                if self.handle:
                    self.engine.lib.dage_run_destroy(self.handle)
                    self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
