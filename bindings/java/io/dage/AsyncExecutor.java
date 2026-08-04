package io.dage;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface AsyncExecutor {
    /**
     * Return a stage whose value is a UTF-8 JSON document.
     *
     * <p>The callback must not block. Use the supplied context to poll native cancellation and to
     * commit declared effects before completing.</p>
     */
    CompletionStage<String> execute(AsyncExecutionContext context, String inputJson);
}
