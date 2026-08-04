using System;
using System.IO;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Dage;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        string vectors = args.Length == 1
            ? args[0]
            : Path.Combine("tests", "conformance", "bindings");
        string valid = File.ReadAllText(Path.Combine(vectors, "valid.json"));
        string invalid = File.ReadAllText(Path.Combine(vectors, "invalid.json"));
        string asyncTool = File.ReadAllText(Path.Combine(vectors, "async_tool.json"));
        string asyncEffect = File.ReadAllText(Path.Combine(vectors, "async_effect.json"));

        using DageEngine engine = new();
        using (DageWorkflow workflow = engine.Load(valid))
        {
            string graph = workflow.Mermaid();
            if (!graph.Contains("flowchart TD") || !graph.Contains("done-完成"))
                throw new InvalidOperationException("Unexpected Mermaid output");
            using DageRun run = engine.CreateRun(workflow);
            string output = await run.ExecuteAsync("{}");
            if (!output.Contains("基础设施-🚀"))
                throw new InvalidOperationException("Run output lost UTF-8");
        }
        try
        {
            engine.Load(invalid).Dispose();
            throw new InvalidOperationException("Invalid Workflow was accepted");
        }
        catch (DageException) { }

        await ExerciseAsyncExecutors(engine, asyncTool, asyncEffect);
        Console.WriteLine("C# binding conformance passed");
    }

    private static async Task ExerciseAsyncExecutors(
        DageEngine engine, string asyncTool, string asyncEffect)
    {
        engine.RegisterAsyncExecutor(
            "csharp_echo", (context, input) => Task.FromResult(input), TaskScheduler.Default);
        engine.RegisterAsyncExecutor(
            "csharp_fail", (context, input) => Task.FromException<string>(
                new InvalidOperationException("expected C# Executor failure")), TaskScheduler.Default);
        engine.RegisterAsyncExecutor("csharp_effect", (context, input) =>
        {
            if (string.IsNullOrEmpty(context.IdempotencyKey))
                return Task.FromException<string>(new InvalidOperationException("missing key"));
            context.CommitEffect();
            return Task.FromResult(input);
        }, TaskScheduler.Default);

        using (DageWorkflow workflow = engine.Load(
            asyncTool.Replace("__EXECUTOR__", "csharp_echo")))
        using (DageRun run = engine.CreateRun(workflow))
        {
            string output = await run.ExecuteAsync("{\"message\":\"C#-🚀\"}");
            if (!output.Contains("C#-🚀")) throw new InvalidOperationException("async UTF-8 failed");
        }
        using (DageWorkflow workflow = engine.Load(
            asyncTool.Replace("__EXECUTOR__", "csharp_fail")))
        using (DageRun run = engine.CreateRun(workflow))
        {
            try
            {
                await run.ExecuteAsync("{}");
                throw new InvalidOperationException("Executor failure was not translated");
            }
            catch (DageException) { }
        }
        string effect = asyncEffect.Replace("__EXECUTOR__", "csharp_effect");
        using (DageWorkflow workflow = engine.Load(effect))
        using (DageRun run = engine.CreateRun(workflow))
        {
            string output = await run.ExecuteAsync("{\"effect\":true}");
            if (!output.Contains("\"effect\":true")) throw new InvalidOperationException("effect failed");
        }

        TaskCompletionSource started = new(TaskCreationOptions.RunContinuationsAsynchronously);
        TaskCompletionSource seen = new(TaskCreationOptions.RunContinuationsAsynchronously);
        engine.RegisterAsyncExecutor("csharp_wait", async (context, input) =>
        {
            started.SetResult();
            while (!context.IsCancellationRequested) await Task.Delay(1).ConfigureAwait(false);
            seen.SetResult();
            return input;
        }, TaskScheduler.Default);
        using (DageWorkflow workflow = engine.Load(
            asyncTool.Replace("__EXECUTOR__", "csharp_wait")))
        using (DageRun run = engine.CreateRun(workflow))
        using (CancellationTokenSource cancellation = new())
        {
            Task<string> execution = run.ExecuteAsync("{}", cancellation.Token);
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            cancellation.Cancel();
            try
            {
                await execution;
                throw new InvalidOperationException("Cancellation was ignored");
            }
            catch (OperationCanceledException) { }
            await seen.Task.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }
}
