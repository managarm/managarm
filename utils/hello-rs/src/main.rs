use std::time::Duration;

use hel::Executor;

fn main() -> hel::Result<()> {
    let executor = Executor::new()?;
    let executor_clone = executor.clone();

    executor.block_on::<_, hel::Result<()>>(async move {
        let now = hel::Time::now()?;

        println!("Current time: {:?}", now);
        println!("Waiting for 1 second...");

        hel::await_clock(&executor_clone, now + Duration::from_secs(1)).await?;

        println!("Waiting for 2 seconds...");

        hel::await_clock(&executor_clone, now + Duration::from_secs(2)).await?;

        println!("Waiting for 3 seconds...");

        hel::await_clock(&executor_clone, now + Duration::from_secs(3)).await?;

        println!("Done!");

        Ok(())
    })?
}
