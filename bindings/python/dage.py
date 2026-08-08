"""Minimal ctypes binding. Workflow semantics remain in the DAGE C++ core."""
import asyncio
import ctypes
import json
import os
import sys
import threading


_DLL_DIRECTORY_HANDLES = []


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
    def __init__(self, status):
        self.status = status
        super().__init__("DAGE status {}".format(status))


def _default_library():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    names = ["libdage.dll", "dage.dll"] if sys.platform == "win32" else ["libdage.dylib", "libdage.so"]
    directories = (
        os.path.join(root, "build"),
        os.path.join(root, "build-shared"),
        root,
    )
    for name in names:
        for directory in directories:
            path = os.path.join(directory, name)
            if os.path.exists(path):
                return path
    raise DageError("DAGE shared library not found; set DAGE_LIBRARY")


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

    def __init__(self, library=None):
        library = library or os.environ.get("DAGE_LIBRARY") or _default_library()
        _prepare_dll_search(library)
        self.lib = ctypes.CDLL(library)
        self.lib.dage_engine_create.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_engine_create.restype = ctypes.c_int
        self.lib.dage_engine_load.argtypes = [ctypes.c_void_p, _View, ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_engine_load.restype = ctypes.c_int
        self.lib.dage_workflow_destroy.argtypes = [ctypes.c_void_p]
        self.lib.dage_run_create.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                             ctypes.POINTER(ctypes.c_void_p)]
        self.lib.dage_run_create.restype = ctypes.c_int
        self.lib.dage_run_destroy.argtypes = [ctypes.c_void_p]
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
        self._async_lock = threading.Lock()
        self._async_completions = set()
        self._async_futures = set()
        self.lib.dage_engine_destroy.argtypes = [ctypes.c_void_p]
        self.handle = ctypes.c_void_p()
        self._check(self.lib.dage_engine_create(None, ctypes.byref(self.handle)))

    def _check(self, status):
        if status:
            raise DageError(status)

    def load(self, document):
        text = document if isinstance(document, str) else json.dumps(document)
        keepalive, view = _view(text)
        handle = ctypes.c_void_p()
        self._check(self.lib.dage_engine_load(self.handle, view, ctypes.byref(handle)))
        return Workflow(self, handle)

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
                    json.loads(ctypes.string_at(input_value.data, input_value.size).decode("utf-8")))
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
            except BaseException:
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
                    except BaseException:
                        completion.complete(5)

                future = asyncio.run_coroutine_threadsafe(invoke(), event_loop)
                with self._async_lock:
                    self._async_futures.add(future)
                future.add_done_callback(self._discard_async_future)
                return 0
            except BaseException:
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

    def create_run(self):
        handle = ctypes.c_void_p()
        self.engine._check(self.engine.lib.dage_run_create(
            self.engine.handle, self.handle, ctypes.byref(handle)))
        return Run(self.engine, handle)

    def close(self):
        if self.handle:
            self.engine.lib.dage_workflow_destroy(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Run:
    def __init__(self, engine, handle):
        self.engine, self.handle = engine, handle

    def _operation(self, function, value=None):
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

    def checkpoint(self):
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

    def close(self):
        if self.handle:
            self.engine.lib.dage_run_destroy(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
