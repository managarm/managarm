use std::time::Duration;

fn main() -> hel::Result<()> {
    hel::block_on(async {
        println!("Going to sleep for 3 seconds");

        for _ in 0..3 {
            println!("Sleeping...");

            hel::sleep_for(Duration::from_secs(1)).await?;
        }

        println!("Done!");

        Ok(())
    })?
}
