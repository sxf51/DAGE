use dage::{Engine, Error, RunFuture, ThreadSpawner};
use std::fs;
use std::future::Future;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::task::{Context, Poll, Waker};
use std::time::{Duration, Instant};

fn wait(mut future: RunFuture) -> dage::Result<String> {
    let mut context = Context::from_waker(Waker::noop());
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Poll::Ready(value) = Pin::new(&mut future).poll(&mut context) {
            return value;
        }
        assert!(Instant::now() < deadline, "Rust RunFuture timed out");
        std::thread::sleep(Duration::from_millis(1));
    }
}

#[test]
fn shared_binding_conformance() {
    let vectors = std::env::var_os("DAGE_CONFORMANCE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("../../tests/conformance/bindings"));
    let valid = fs::read_to_string(vectors.join("valid.json")).unwrap();
    let invalid = fs::read_to_string(vectors.join("invalid.json")).unwrap();
    let async_tool = fs::read_to_string(vectors.join("async_tool.json")).unwrap();
    let async_effect = fs::read_to_string(vectors.join("async_effect.json")).unwrap();
    let engine = Engine::new().unwrap();
    let workflow = engine.load(&valid).unwrap();
    let graph = workflow.mermaid().unwrap();
    assert!(graph.contains("flowchart TD") && graph.contains("done-完成"));
    assert!(engine.load(&invalid).is_err());

    let spawner = Arc::new(ThreadSpawner);
    engine
        .register_async_executor(
            "rust_echo",
            |_context, input| Box::pin(async move { Ok(input) }),
            spawner.clone(),
        )
        .unwrap();
    engine
        .register_async_executor(
            "rust_fail",
            |_context, _input| Box::pin(async { Err(Error(5)) }),
            spawner.clone(),
        )
        .unwrap();
    engine
        .register_async_executor(
            "rust_effect",
            |context, input| {
                Box::pin(async move {
                    assert!(!context.idempotency_key.is_empty());
                    context.commit_effect()?;
                    Ok(input)
                })
            },
            spawner.clone(),
        )
        .unwrap();

    let workflow_json = |executor: &str| async_tool.replace("__EXECUTOR__", executor);
    let workflow = engine.load(&workflow_json("rust_echo")).unwrap();
    let run = engine.create_run(&workflow).unwrap();
    let output = wait(
        run.execute_async("{\"message\":\"Rust-🚀\"}".into(), spawner.clone())
            .unwrap(),
    )
    .unwrap();
    assert!(output.contains("Rust-🚀"));

    let workflow = engine.load(&workflow_json("rust_fail")).unwrap();
    let run = engine.create_run(&workflow).unwrap();
    assert_eq!(
        wait(run.execute_async("{}".into(), spawner.clone()).unwrap()),
        Err(Error(5))
    );

    let effect = async_effect.replace("__EXECUTOR__", "rust_effect");
    let workflow = engine.load(&effect).unwrap();
    let run = engine.create_run(&workflow).unwrap();
    assert!(wait(
        run.execute_async("{\"effect\":true}".into(), spawner.clone())
            .unwrap()
    )
    .unwrap()
    .contains("\"effect\":true"));

    let started = Arc::new(AtomicBool::new(false));
    let seen = Arc::new(AtomicBool::new(false));
    let started_callback = started.clone();
    let seen_callback = seen.clone();
    engine
        .register_async_executor(
            "rust_wait",
            move |context, input| {
                let started = started_callback.clone();
                let seen = seen_callback.clone();
                Box::pin(async move {
                    started.store(true, Ordering::Release);
                    while !context.is_cancelled() {
                        std::thread::yield_now();
                    }
                    seen.store(true, Ordering::Release);
                    Ok(input)
                })
            },
            spawner.clone(),
        )
        .unwrap();
    let workflow = engine.load(&workflow_json("rust_wait")).unwrap();
    let run = engine.create_run(&workflow).unwrap();
    let future = run.execute_async("{}".into(), spawner).unwrap();
    let deadline = Instant::now() + Duration::from_secs(5);
    while !started.load(Ordering::Acquire) {
        assert!(
            Instant::now() < deadline,
            "Rust async Executor did not start"
        );
        std::thread::yield_now();
    }
    drop(future);
    while !seen.load(Ordering::Acquire) {
        assert!(
            Instant::now() < deadline,
            "Rust async Executor did not observe cancellation"
        );
        std::thread::yield_now();
    }
}
