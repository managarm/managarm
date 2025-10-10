#!/usr/bin/python3 -u

import asyncio
import base64
import json

DEBUG = False


class CiBoot:
    def __init__(self, pipe):
        self.pipe = pipe

    async def run(self):
        print("Ready for commands")
        self._emit({"m": "ready"})
        for line in self.pipe:
            if DEBUG:
                print("Received line:", repr(line))
            try:
                msg = json.loads(line)
                if msg["m"] == "launch":
                    script = msg["script"]
                    await self._launch(script)
                else:
                    raise RuntimeError("Bad message type: " + msg["m"])
            except Exception as e:
                print("Error:", str(e))
                self._emit({"m": "error", "text": str(e)})

    async def _launch(self, script):
        print("Launching script:", repr(script))
        process = await asyncio.create_subprocess_exec(
            "/usr/bin/bash",
            "-c",
            script,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )

        await asyncio.gather(
            self._handle_output("stdout", process.stdout),
            self._handle_output("stderr", process.stderr),
            process.wait(),
        )

        print(f"Script terminated with {process.returncode}")
        self._emit({"m": "exit", "exitcode": process.returncode})

    async def _handle_output(self, kind, stream):
        async for line in stream:
            print(f"{kind}: " + line.rstrip().decode("utf8", errors="backslashreplace"))
            self._emit({"m": kind, "data": base64.b64encode(line).decode("ascii")})

    def _emit(self, msg):
        line = json.dumps(msg)
        self.pipe.write(line.encode("utf8") + b"\n")


async def main():
    with open("/dev/ttyS0", "rb+", buffering=0) as f:
        ciboot = CiBoot(f)
        await ciboot.run()


if __name__ == "__main__":
    asyncio.run(main())
