use anyhow::Result;

fn main() -> Result<()> {
    hel::block_on(async {
        println!("sif: Hello, world!");

        let cmdline = managarm::kerncfg::get_cmdline().await?;
        println!("sif: kernel command line is {cmdline:?}");

        let enabled = cmdline.split_ascii_whitespace().any(|opt| opt == "sif");
        if enabled {
            println!("sif: enabled on the kernel command line");
        } else {
            println!("sif: disabled on the kernel command line");
        }

        Ok(())
    })?
}
