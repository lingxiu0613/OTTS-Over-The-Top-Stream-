#!/usr/bin/env python3

import argparse
import subprocess
import sys


def run(command: list[str]) -> int:
    print(" ".join(command))
    process = subprocess.run(command, check=False)
    return process.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description="RTMP smoke helper for OTTS")
    parser.add_argument("--input", default="sample.mp4", help="Media file to publish")
    parser.add_argument("--url", default="rtmp://127.0.0.1/live/demo", help="RTMP URL")
    parser.add_argument(
        "--mode",
        choices=["publish", "play"],
        required=True,
        help="Run a publish or play smoke command",
    )
    args = parser.parse_args()

    if args.mode == "publish":
        command = [
            "ffmpeg",
            "-re",
            "-stream_loop",
            "-1",
            "-i",
            args.input,
            "-c",
            "copy",
            "-f",
            "flv",
            args.url,
        ]
    else:
        command = ["ffplay", args.url]

    return run(command)


if __name__ == "__main__":
    sys.exit(main())
