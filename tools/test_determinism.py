#!/usr/bin/env python3
# Copyright (c) 2020 Graphcore Ltd. All rights reserved.
"""Verify that compilation is deterministic.

This tool runs a command multiple times and checks that the archive generated by
the engine is always the same. If any difference is found then the test is failed.
"""
import argparse
import asyncio
import os
import logging
import json
import subprocess
import sys
import tempfile


LOG = logging.getLogger()
"""The logger instance to use throughout this module."""


class ColouredLoggingFormatter(logging.Formatter):
    """A `logging.Formatter` subclass that colours the level name of the record."""
    RESET = "\033[0m"
    LOG_LEVEL_TO_COLOR = {
        "DEBUG": "\033[1;34m",
        "INFO": "\033[1;37m",
        "WARNING": "\033[1;33m",
        "ERROR": "\033[1;31m",
        "CRITICAL": "\033[1;35m",
    }

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def format(self, record) -> str:
        if sys.__stdout__.isatty():
            levelname = record.levelname
            colour = self.LOG_LEVEL_TO_COLOR.get(levelname, None)
            if colour is not None:
                record.levelname = colour + levelname + self.RESET
        return super().format(record)


class SubprocessError(subprocess.CalledProcessError):
    """An extension of `subprocess.CalledProcessError` that prints stdout and stderr."""
    def __init__(self, cmd, returncode, stdout, stderr, kwargs):
        super().__init__(returncode, cmd, stdout, stderr)
        self.cmd = " ".join(cmd)
        self.kwargs = kwargs
    def __str__(self):
        string = super().__str__()
        if self.stdout:
            string += f"\nstdout: {self.stdout}"
        if self.stderr:
            string += f"\nstderr: {self.stderr}"
        if self.kwargs and LOG.getEffectiveLevel() <= logging.DEBUG:
            string += f"\nkwargs: {self.kwargs}"
        return string
    @classmethod
    def from_result(cls, result, kwargs):
        """Construct a SubprocessError instance from a subprocess.CompletedProcess object."""
        assert isinstance(result, subprocess.CompletedProcess)
        return cls(result.args, result.returncode, result.stdout, result.stderr, kwargs)


async def run_async_subprocess(cmd, *args, **kwargs) -> subprocess.CompletedProcess:
    """Run a subprocess asynchronously."""
    # Support some options from `subprocess.run`.
    check = kwargs.pop("check", False)
    input_ = kwargs.pop("input", None)
    decode = kwargs.pop("universal_newlines", True)
    # Use sensible defaults for this script.
    kwargs.setdefault("stdout", asyncio.subprocess.PIPE)
    kwargs.setdefault("stderr", asyncio.subprocess.PIPE)
    # Spawn the subprocess and wait for it to complete asynchronously.
    proc = await asyncio.create_subprocess_exec(*cmd, *args, **kwargs)
    stdout, stderr = await proc.communicate(input_)
    if decode:
        stdout = stdout.strip().decode("utf-8")
        stderr = stderr.strip().decode("utf-8")
    # Make the return value look like that of `subprocess.run`.
    result = subprocess.CompletedProcess(cmd, proc.returncode, stdout, stderr)
    if check and result.returncode != 0:
        raise SubprocessError.from_result(result, kwargs)
    LOG.debug(f"Ran: '{' '.join(cmd)}' -> '{result.stdout}'")
    return result


def cli():
    """Define the command line interface of the script."""
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("--output", help="the directory to store the archives")
    parser.add_argument("--repeats", type=int, default=2, help="the number of compilations to run")
    parser.add_argument("--overwrite", action="store_true", help="ignore existing archives")
    parser.add_argument("--log-level", choices=("debug", "info", "warning", "error", "critical"),
                        default="info", help="the severity of log messages to print")
    return parser


def main(*args, **kwargs):
    """Entrypoint of the script."""
    parser = cli()
    opts, command = parser.parse_known_args(*args, **kwargs)

    # Prepare the logger.
    global LOG
    LOG.setLevel(getattr(logging, opts.log_level.upper()))
    handler = logging.StreamHandler()
    handler.setFormatter(ColouredLoggingFormatter("%(levelname)s: %(message)s"))
    LOG.addHandler(handler)

    LOG.debug(f"opts={opts}")
    LOG.debug(f"command={command}")

    ref = None # Keep the temporary directory alive for the duration of the test.
    if opts.output:
        out = opts.output
        os.makedirs(out, exist_ok=True)
    else:
        ref = tempfile.TemporaryDirectory()
        out = ref.name
        logging.debug(f"Using temporary directory: {out}")

    # Generate the names of the archives.
    archive_names = {os.path.join(out, f"archive{i}.a") for i in range(opts.repeats)}
    archives_to_generate = archive_names if opts.overwrite else {
        archive
        for archive in archive_names
        if not os.path.exists(archive)
    }

    # Construct the environment variables needed to generate the archive
    # without clobbering any of the users environment variables.
    poplar_engine_options = os.environ.get("POPLAR_ENGINE_OPTIONS", "{}")
    engine_options_dict = json.loads(poplar_engine_options)

    # Check that the user hasn't already specified the engine options we need.
    if "target.saveArchive" in engine_options_dict:
        raise RuntimeError("POPLAR_ENGINE_OPTIONS already contains 'target.saveArchive'")

    envs = []
    for archive in archives_to_generate:
        engine_options_dict["target.saveArchive"] = archive
        env_copy = dict(os.environ)
        env_copy["POPLAR_ENGINE_OPTIONS"] = json.dumps(engine_options_dict)
        envs.append(env_copy)

    # Run the commands asynchronously.
    coros = [
        run_async_subprocess(command, check=True, env=env)
        for archive, env in zip(archives_to_generate, envs)
    ]
    LOG.info(f"Running {len(coros)} commands")
    event_loop = asyncio.get_event_loop()
    event_loop.run_until_complete(asyncio.gather(*coros))

    # Check that the archives were created
    missing_archives = [
        archive
        for archive in archive_names
        if not os.path.exists(archive)
    ]
    if missing_archives:
        raise RuntimeError(f"{len(missing_archives)} compilations failed")

    LOG.debug(f"Generated archives: {archives_to_generate}")

    # Check that the hash of the archives is the same.
    hash_results = event_loop.run_until_complete(asyncio.gather(*[
        run_async_subprocess(["cksum", archive], check=True)
        for archive in archive_names
    ]))

    unique_hashes = {result.stdout.split(" ", 1)[0] for result in hash_results}
    if len(unique_hashes) != 1:
        LOG.error(f"Non-deterministic compilation: {len(unique_hashes)} different archives")
        sys.exit(1)

    LOG.info("Passed :-)")


if __name__ == "__main__":
    main()