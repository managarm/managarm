use hel::Executor;

fn main() -> hel::Result<()> {
    let executor = Executor::new()?;

    executor.block_on(async {
        let clock = hel::get_clock()?;

        hel::submission::await_clock(&executor, clock + 1_000_000_000).await?;
        hel::submission::await_clock(&executor, clock + 2_000_000_000).await?;
        hel::submission::await_clock(&executor, clock + 3_000_000_000).await?;

        Ok(())
    })
}
