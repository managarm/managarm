use std::time::Duration;

use hel::Executor;

fn main() -> hel::Result<()> {
    let executor = Executor::new()?;
    let executor_clone = executor.clone();

    executor.block_on::<_, hel::Result<()>>(async move {
        println!("Sleeping for 1 second...");

        hel::sleep_for(&executor_clone, Duration::from_secs(1)).await?;

        println!("Sleeping for 2 seconds...");

        hel::sleep_for(&executor_clone, Duration::from_secs(1)).await?;

        println!("Sleeping for 3 seconds...");

        hel::sleep_for(&executor_clone, Duration::from_secs(1)).await?;

        println!("Done!");

        Ok(())
    })?
}
