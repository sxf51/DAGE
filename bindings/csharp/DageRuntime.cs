using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace Dage;

internal enum Status : int
{
    Ok = 0,
    InvalidArgument = 1,
    ExecutionError = 5,
    Cancelled = 6,
    BufferTooSmall = 8
}

public sealed class DageException : Exception
{
    public int Status { get; }
    internal DageException(Status status) : base($"DAGE status {(int)status}") => Status = (int)status;
}

[StructLayout(LayoutKind.Sequential)]
internal readonly struct StringView
{
    internal readonly IntPtr Data;
    internal readonly UIntPtr Size;
    internal StringView(IntPtr data, int size) { Data = data; Size = (UIntPtr)size; }
}

[StructLayout(LayoutKind.Sequential)]
internal struct OwnedBuffer
{
    internal IntPtr Data;
    internal UIntPtr Size;
    internal IntPtr Release;
    internal IntPtr ReleaseUserdata;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeExecutionContext
{
    internal uint StructSize;
    internal StringView RunId;
    internal StringView NodeId;
    internal StringView NodeType;
    internal uint Attempt;
    internal StringView IdempotencyKey;
    internal StringView RunMode;
    internal ulong DeadlineRemainingMs;
    internal IntPtr CommitEffect;
    internal IntPtr CommitEffectUserdata;
    internal IntPtr IsCancelled;
    internal IntPtr CancellationUserdata;
}

internal sealed class EngineHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    private EngineHandle() : base(true) { }
    protected override bool ReleaseHandle() { Native.EngineDestroy(handle); return true; }
}

internal sealed class WorkflowHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    private WorkflowHandle() : base(true) { }
    protected override bool ReleaseHandle() { Native.WorkflowDestroy(handle); return true; }
}

internal sealed class RunHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    private RunHandle() : base(true) { }
    protected override bool ReleaseHandle() { Native.RunDestroy(handle); return true; }
}

internal static class Native
{
    private const string Library = "libdage";
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate Status AsyncExecutorCallback(
        IntPtr context, StringView input, IntPtr completion, IntPtr userdata);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void UserdataDestroy(IntPtr userdata);

    static Native()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, assembly, path) =>
        {
            string? configured = Environment.GetEnvironmentVariable("DAGE_LIBRARY");
            return name == Library && configured is not null
                ? NativeLibrary.Load(configured)
                : IntPtr.Zero;
        });
    }

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_engine_create")]
    internal static extern Status EngineCreate(IntPtr options, out EngineHandle engine);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_engine_destroy")]
    internal static extern void EngineDestroy(IntPtr engine);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_engine_load")]
    internal static extern Status EngineLoad(EngineHandle engine, StringView json, out WorkflowHandle workflow);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_engine_register_async_executor")]
    internal static extern Status RegisterAsyncExecutor(
        EngineHandle engine, StringView name, AsyncExecutorCallback callback,
        IntPtr userdata, UserdataDestroy destroy);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_workflow_destroy")]
    internal static extern void WorkflowDestroy(IntPtr workflow);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_workflow_export_mermaid")]
    internal static extern Status ExportMermaid(
        WorkflowHandle workflow, IntPtr buffer, UIntPtr size, out UIntPtr required);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_run_create")]
    internal static extern Status RunCreate(
        EngineHandle engine, WorkflowHandle workflow, out RunHandle run);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_run_destroy")]
    internal static extern void RunDestroy(IntPtr run);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_run_execute")]
    internal static extern Status RunExecute(
        RunHandle run, StringView input, IntPtr output, UIntPtr size, out UIntPtr required);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_run_cancel_with_reason")]
    internal static extern Status RunCancel(RunHandle run, StringView reason);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_executor_complete")]
    internal static extern Status ExecutorComplete(IntPtr completion, Status status, IntPtr output);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_executor_abandon")]
    internal static extern void ExecutorAbandon(IntPtr completion);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_executor_completion_is_cancelled")]
    [return: MarshalAs(UnmanagedType.U1)]
    internal static extern bool ExecutorIsCancelled(IntPtr completion);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "dage_executor_completion_commit_effect")]
    internal static extern Status ExecutorCommitEffect(IntPtr completion);
}

public sealed class AsyncExecutionContext
{
    private readonly object gate = new();
    private IntPtr completion;

    internal AsyncExecutionContext(IntPtr completion, NativeExecutionContext context)
    {
        this.completion = completion;
        RunId = Utf8.Read(context.RunId);
        NodeId = Utf8.Read(context.NodeId);
        NodeType = Utf8.Read(context.NodeType);
        Attempt = context.Attempt;
        IdempotencyKey = Utf8.Read(context.IdempotencyKey);
        RunMode = Utf8.Read(context.RunMode);
        DeadlineRemainingMs = context.DeadlineRemainingMs;
    }

    public string RunId { get; }
    public string NodeId { get; }
    public string NodeType { get; }
    public uint Attempt { get; }
    public string IdempotencyKey { get; }
    public string RunMode { get; }
    public ulong DeadlineRemainingMs { get; }

    public bool IsCancellationRequested
    {
        get { lock (gate) return completion == IntPtr.Zero || Native.ExecutorIsCancelled(completion); }
    }

    public void CommitEffect()
    {
        lock (gate)
        {
            if (completion == IntPtr.Zero) throw new InvalidOperationException("Completion is closed");
            Check(Native.ExecutorCommitEffect(completion));
        }
    }

    internal unsafe void Complete(string output)
    {
        ArgumentNullException.ThrowIfNull(output);
        lock (gate)
        {
            IntPtr handle = completion;
            completion = IntPtr.Zero;
            if (handle == IntPtr.Zero) return;
            byte[] bytes = Encoding.UTF8.GetBytes(output);
            fixed (byte* data = bytes)
            {
                OwnedBuffer buffer = new()
                {
                    Data = (IntPtr)data,
                    Size = (UIntPtr)bytes.Length
                };
                Check(Native.ExecutorComplete(handle, Status.Ok, (IntPtr)(&buffer)));
            }
        }
    }

    internal void Fail()
    {
        lock (gate)
        {
            IntPtr handle = completion;
            completion = IntPtr.Zero;
            if (handle != IntPtr.Zero)
                Check(Native.ExecutorComplete(handle, Status.ExecutionError, IntPtr.Zero));
        }
    }

    internal void Abandon()
    {
        lock (gate)
        {
            IntPtr handle = completion;
            completion = IntPtr.Zero;
            if (handle != IntPtr.Zero) Native.ExecutorAbandon(handle);
        }
    }

    private static void Check(Status status)
    {
        if (status != Status.Ok) throw new DageException(status);
    }
}

internal sealed class AsyncRegistration
{
    internal AsyncRegistration(Func<AsyncExecutionContext, string, Task<string>> callback,
            TaskScheduler scheduler)
    {
        Callback = callback;
        Scheduler = scheduler;
    }
    internal Func<AsyncExecutionContext, string, Task<string>> Callback { get; }
    internal TaskScheduler Scheduler { get; }
}

internal static class Utf8
{
    internal static string Read(StringView view)
    {
        int size = checked((int)view.Size.ToUInt64());
        if (size == 0) return string.Empty;
        byte[] bytes = new byte[size];
        Marshal.Copy(view.Data, bytes, 0, size);
        return Encoding.UTF8.GetString(bytes);
    }
}

public sealed class DageEngine : IDisposable
{
    private readonly object gate = new();
    private EngineHandle? handle;
    private int liveRuns;
    private static readonly Native.AsyncExecutorCallback AsyncThunk = InvokeAsync;
    private static readonly Native.UserdataDestroy DestroyThunk = DestroyRegistration;

    public DageEngine()
    {
        Check(Native.EngineCreate(IntPtr.Zero, out EngineHandle created));
        handle = created;
    }

    public unsafe DageWorkflow Load(string document)
    {
        ArgumentNullException.ThrowIfNull(document);
        lock (gate)
        {
            EngineHandle engine = EnsureOpen();
            byte[] bytes = Encoding.UTF8.GetBytes(document);
            fixed (byte* data = bytes)
            {
                Check(Native.EngineLoad(
                    engine, new StringView((IntPtr)data, bytes.Length), out WorkflowHandle workflow));
                return new DageWorkflow(this, workflow);
            }
        }
    }

    public unsafe void RegisterAsyncExecutor(string name,
            Func<AsyncExecutionContext, string, Task<string>> callback, TaskScheduler scheduler)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        ArgumentNullException.ThrowIfNull(callback);
        ArgumentNullException.ThrowIfNull(scheduler);
        GCHandle registration = GCHandle.Alloc(new AsyncRegistration(callback, scheduler));
        bool transferred = false;
        try
        {
            lock (gate)
            {
                EngineHandle engine = EnsureOpen();
                byte[] bytes = Encoding.UTF8.GetBytes(name);
                fixed (byte* data = bytes)
                {
                    transferred = true;
                    Check(Native.RegisterAsyncExecutor(
                        engine, new StringView((IntPtr)data, bytes.Length), AsyncThunk,
                        GCHandle.ToIntPtr(registration), DestroyThunk));
                }
            }
        }
        catch
        {
            if (!transferred) registration.Free();
            throw;
        }
    }

    public DageRun CreateRun(DageWorkflow workflow)
    {
        ArgumentNullException.ThrowIfNull(workflow);
        lock (gate)
        {
            if (!ReferenceEquals(workflow.Owner, this))
                throw new ArgumentException("Workflow belongs to another Engine", nameof(workflow));
            Check(Native.RunCreate(EnsureOpen(), workflow.Handle, out RunHandle run));
            liveRuns++;
            return new DageRun(this, workflow, run);
        }
    }

    internal void ReleaseRun()
    {
        lock (gate)
        {
            if (liveRuns <= 0) throw new InvalidOperationException("Run ownership underflow");
            liveRuns--;
        }
    }

    public void Dispose()
    {
        lock (gate)
        {
            if (liveRuns != 0) throw new InvalidOperationException("Engine has live Runs");
            handle?.Dispose();
            handle = null;
        }
    }

    private EngineHandle EnsureOpen() =>
        handle is { IsInvalid: false, IsClosed: false } value
            ? value : throw new ObjectDisposedException(nameof(DageEngine));

    private static Status InvokeAsync(
            IntPtr contextPointer, StringView input, IntPtr completion, IntPtr userdata)
    {
        AsyncExecutionContext? context = null;
        try
        {
            AsyncRegistration registration =
                (AsyncRegistration)(GCHandle.FromIntPtr(userdata).Target
                    ?? throw new InvalidOperationException("Async registration was released"));
            context = new AsyncExecutionContext(
                completion, Marshal.PtrToStructure<NativeExecutionContext>(contextPointer));
            string inputJson = Utf8.Read(input);
            Task.Factory.StartNew(
                () => registration.Callback(context, inputJson),
                CancellationToken.None,
                TaskCreationOptions.DenyChildAttach,
                registration.Scheduler).Unwrap().ContinueWith(
                    task =>
                    {
                        if (task.IsCanceled) context.Abandon();
                        else if (task.IsFaulted) context.Fail();
                        else context.Complete(task.Result);
                    },
                    CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default);
            return Status.Ok;
        }
        catch
        {
            context?.Abandon();
            return context is null ? Status.ExecutionError : Status.Ok;
        }
    }

    private static void DestroyRegistration(IntPtr userdata)
    {
        try { GCHandle.FromIntPtr(userdata).Free(); }
        catch { /* Never let managed cleanup cross the native callback boundary. */ }
    }

    internal static void Check(Status status)
    {
        if (status != Status.Ok) throw new DageException(status);
    }
}

public sealed class DageWorkflow : IDisposable
{
    private WorkflowHandle? handle;
    internal DageWorkflow(DageEngine owner, WorkflowHandle handle)
    {
        Owner = owner;
        this.handle = handle;
    }
    internal DageEngine Owner { get; }
    internal WorkflowHandle Handle =>
        handle is { IsInvalid: false, IsClosed: false } value
            ? value : throw new ObjectDisposedException(nameof(DageWorkflow));

    public string Mermaid()
    {
        Status first = Native.ExportMermaid(Handle, IntPtr.Zero, UIntPtr.Zero, out UIntPtr required);
        if (first != Status.BufferTooSmall) DageEngine.Check(first);
        IntPtr output = Marshal.AllocHGlobal(checked((int)required.ToUInt64()));
        try
        {
            DageEngine.Check(Native.ExportMermaid(Handle, output, required, out required));
            return Marshal.PtrToStringUTF8(output) ?? string.Empty;
        }
        finally { Marshal.FreeHGlobal(output); }
    }

    public void Dispose()
    {
        handle?.Dispose();
        handle = null;
    }
}

public sealed class DageRun : IDisposable
{
    private readonly object gate = new();
    private readonly DageEngine engine;
    private readonly DageWorkflow workflow;
    private RunHandle? handle;
    private Task<string>? active;

    internal DageRun(DageEngine engine, DageWorkflow workflow, RunHandle handle)
    {
        this.engine = engine;
        this.workflow = workflow;
        this.handle = handle;
    }

    public Task<string> ExecuteAsync(
            string inputJson, CancellationToken cancellationToken = default,
            TaskScheduler? scheduler = null)
    {
        ArgumentNullException.ThrowIfNull(inputJson);
        lock (gate)
        {
            EnsureOpen();
            if (active is not null) throw new InvalidOperationException("Run is already executing");
            TaskScheduler target = scheduler ?? TaskScheduler.Default;
            active = Task.Factory.StartNew(
                () => ExecuteCore(inputJson, cancellationToken),
                CancellationToken.None,
                TaskCreationOptions.LongRunning,
                target);
            return active;
        }
    }

    public unsafe void Cancel(string reason = "C# Run cancelled")
    {
        ArgumentNullException.ThrowIfNull(reason);
        lock (gate)
        {
            RunHandle run = EnsureOpen();
            byte[] bytes = Encoding.UTF8.GetBytes(reason);
            fixed (byte* data = bytes)
                DageEngine.Check(Native.RunCancel(
                    run, new StringView((IntPtr)data, bytes.Length)));
        }
    }

    private unsafe string ExecuteCore(string inputJson, CancellationToken token)
    {
        using CancellationTokenRegistration cancellation =
            token.Register(() => Cancel("C# CancellationToken cancelled"));
        try
        {
            RunHandle run;
            lock (gate) run = EnsureOpen();
            byte[] input = Encoding.UTF8.GetBytes(inputJson);
            fixed (byte* inputData = input)
            {
                StringView view = new((IntPtr)inputData, input.Length);
                Status first = Native.RunExecute(run, view, IntPtr.Zero, UIntPtr.Zero, out UIntPtr required);
                if (first == Status.Cancelled) throw new OperationCanceledException(token);
                if (first != Status.BufferTooSmall) DageEngine.Check(first);
                IntPtr output = Marshal.AllocHGlobal(checked((int)required.ToUInt64()));
                try
                {
                    Status status = Native.RunExecute(run, view, output, required, out required);
                    if (status == Status.Cancelled) throw new OperationCanceledException(token);
                    DageEngine.Check(status);
                    return Marshal.PtrToStringUTF8(output) ?? string.Empty;
                }
                finally { Marshal.FreeHGlobal(output); }
            }
        }
        finally
        {
            lock (gate) active = null;
        }
    }

    public void Dispose()
    {
        Task<string>? running;
        lock (gate)
        {
            if (handle is null) return;
            running = active;
            if (running is not null) Cancel("C# Run.Dispose");
        }
        if (running is not null)
        {
            try { running.GetAwaiter().GetResult(); }
            catch (OperationCanceledException) { }
            catch (DageException) { }
        }
        lock (gate)
        {
            handle?.Dispose();
            handle = null;
            engine.ReleaseRun();
            GC.KeepAlive(workflow);
        }
    }

    private RunHandle EnsureOpen() =>
        handle is { IsInvalid: false, IsClosed: false } value
            ? value : throw new ObjectDisposedException(nameof(DageRun));
}
