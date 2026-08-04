package io.dage;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public final class Smoke {
    public static void main(String[] args) throws Exception {
        Path vectors = args.length == 1
            ? Path.of(args[0])
            : Path.of("tests", "conformance", "bindings");
        String valid = Files.readString(vectors.resolve("valid.json"), StandardCharsets.UTF_8);
        String invalid = Files.readString(vectors.resolve("invalid.json"), StandardCharsets.UTF_8);
        String asyncTool = Files.readString(vectors.resolve("async_tool.json"), StandardCharsets.UTF_8);
        String asyncEffect = Files.readString(vectors.resolve("async_effect.json"), StandardCharsets.UTF_8);
        ExecutorService runExecutor = Executors.newSingleThreadExecutor();
        ExecutorService callbackExecutor = Executors.newSingleThreadExecutor();
        try (Engine engine = new Engine()) {
            engine.registerAsyncExecutor(
                "java_async_echo",
                (context, input) -> CompletableFuture.completedFuture(input),
                callbackExecutor);
            engine.registerAsyncExecutor(
                "java_async_fail",
                (context, input) -> CompletableFuture.failedFuture(
                    new IllegalStateException("expected Java Executor failure")),
                callbackExecutor);
            engine.registerAsyncExecutor("java_async_effect", (context, input) -> {
                if (context.idempotencyKey().isEmpty()) {
                    return CompletableFuture.failedFuture(
                        new AssertionError("missing idempotency key"));
                }
                context.commitEffect();
                return CompletableFuture.completedFuture(input);
            }, callbackExecutor);
            try (Workflow workflow = engine.load(valid)) {
                String graph = workflow.mermaid();
                if (!graph.contains("flowchart TD") || !graph.contains("done-完成")) {
                    throw new AssertionError("unexpected Mermaid output");
                }
                try (Run run = engine.createRun(workflow)) {
                    String output = run.executeAsync("{}", runExecutor).get(5, TimeUnit.SECONDS);
                    if (!output.contains("\"conformance\":true")
                            || !output.contains("基础设施-🚀")) {
                        throw new AssertionError("unexpected Run output");
                    }
                }
                Run ownershipProbe = engine.createRun(workflow);
                try {
                    assertEngineRejectsLiveRun(engine);
                } finally {
                    ownershipProbe.close();
                }
                try (Run run = engine.createRun(workflow)) {
                    run.cancel("Java conformance pre-cancel");
                    try {
                        run.executeAsync("{}", runExecutor).get(5, TimeUnit.SECONDS);
                        throw new AssertionError("cancelled Run completed successfully");
                    } catch (ExecutionException expected) {
                        if (!(expected.getCause() instanceof DageException)) throw expected;
                    }
                }
            }
            exerciseAsyncExecutors(engine, runExecutor, callbackExecutor, asyncTool, asyncEffect);
            try {
                engine.load(invalid).close();
                throw new AssertionError("invalid Workflow was accepted");
            } catch (DageException expected) {
                // The binding must translate native validation failure.
            }
        } finally {
            runExecutor.shutdownNow();
            callbackExecutor.shutdownNow();
            if (!runExecutor.awaitTermination(5, TimeUnit.SECONDS)
                    || !callbackExecutor.awaitTermination(5, TimeUnit.SECONDS))
                throw new AssertionError("Java conformance executor did not terminate");
        }
        System.out.println("Java binding conformance passed");
    }

    private static void assertEngineRejectsLiveRun(Engine engine) {
        try {
            engine.close();
            throw new AssertionError("Engine closed with a live Run");
        } catch (IllegalStateException expected) {
            // C Run error handling retains the Engine handle.
        }
    }

    private static void exerciseAsyncExecutors(
            Engine engine, ExecutorService runExecutor, ExecutorService callbackExecutor,
            String asyncTool, String asyncEffect)
            throws Exception {
        try (Workflow workflow = engine.load(asyncTool.replace("__EXECUTOR__", "java_async_echo"));
                Run run = engine.createRun(workflow)) {
            String output = run.executeAsync("{\"message\":\"Java-🚀\"}", runExecutor)
                .get(5, TimeUnit.SECONDS);
            if (!output.contains("Java-🚀")) throw new AssertionError("async output lost UTF-8");
        }
        try (Workflow workflow = engine.load(asyncTool.replace("__EXECUTOR__", "java_async_fail"));
                Run run = engine.createRun(workflow)) {
            try {
                run.executeAsync("{}", runExecutor).get(5, TimeUnit.SECONDS);
                throw new AssertionError("async Executor exception was not translated");
            } catch (ExecutionException expected) {
                if (!(expected.getCause() instanceof DageException)) throw expected;
            }
        }
        String effectWorkflow = asyncEffect.replace("__EXECUTOR__", "java_async_effect");
        try (Workflow workflow = engine.load(effectWorkflow);
                Run run = engine.createRun(workflow)) {
            String output = run.executeAsync("{\"effect\":true}", runExecutor)
                .get(5, TimeUnit.SECONDS);
            if (!output.contains("\"effect\":true")) {
                throw new AssertionError("effect completion failed");
            }
        }

        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch cancellationSeen = new CountDownLatch(1);
        engine.registerAsyncExecutor("java_async_wait", (context, input) -> {
            CompletableFuture<String> result = new CompletableFuture<>();
            started.countDown();
            callbackExecutor.execute(() -> {
                while (!context.isCancelled()) Thread.onSpinWait();
                cancellationSeen.countDown();
                result.complete(input);
            });
            return result;
        }, callbackExecutor);
        try (Workflow workflow = engine.load(asyncTool.replace("__EXECUTOR__", "java_async_wait"));
                Run run = engine.createRun(workflow)) {
            CompletableFuture<String> future = run.executeAsync("{}", runExecutor);
            if (!started.await(5, TimeUnit.SECONDS)) {
                throw new AssertionError("async Executor did not start");
            }
            if (!future.cancel(false)) throw new AssertionError("Run cancellation was rejected");
            if (!cancellationSeen.await(5, TimeUnit.SECONDS)) {
                throw new AssertionError("async Executor did not observe cancellation");
            }
        }
    }
}
