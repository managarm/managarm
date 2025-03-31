use std::time::Duration;

use hel::Executor;

fn main() -> hel::Result<()> {
    let executor = Executor::new()?;

    executor.block_on(async {
        let now = hel::Time::now()?;

        hel::await_clock(&executor, now + Duration::from_secs(1)).await?;
        hel::await_clock(&executor, now + Duration::from_secs(2)).await?;
        hel::await_clock(&executor, now + Duration::from_secs(3)).await?;

        Ok(())
    })
}
