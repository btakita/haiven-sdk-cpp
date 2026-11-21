"""Tests for the shadow builder request and encrypted output contract."""

import argparse
import contextlib
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build


class ShadowBuildTests(unittest.TestCase):
    def test_request_limits_sources_and_runner_classes(self):
        request = {
            "target": "cpp", "source_sha": "a" * 40, "dotnet_sha": "",
            "version": "", "request_id": "123", "recipient": "age1" + "p" * 58,
        }
        self.assertEqual(build.request(json.dumps(request)), request)
        for replace in (
            {"source_sha": "refs/heads/main"}, {"request_id": "123\nother=value"},
            {"target": "dotnet", "dotnet_sha": "b" * 40}, {"target": "unity-core"},
            {"target": "bossroom"}, {"target": "cli", "version": "main"},
            {"recipient": "AGE-SECRET-KEY-EXAMPLE"},
        ):
            with self.subTest(replace=replace), self.assertRaises(ValueError):
                build.request(json.dumps({**request, **replace}))
        licensed = {**request, "target": "bossroom"}
        self.assertEqual(build.request(json.dumps(licensed), private=True), licensed)

    @unittest.skipUnless(shutil.which("age") and shutil.which("age-keygen"), "age unavailable")
    def test_encrypts_diagnostics_and_retains_owned_payload_only_on_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            key = root / "age-key"
            subprocess.run(("age-keygen", "-o", key), check=True, capture_output=True)
            recipient = subprocess.check_output(("age-keygen", "-y", key), text=True).strip()
            request = {
                "target": "cpp", "source_sha": "a" * 40, "dotnet_sha": "",
                "version": "", "request_id": "123", "recipient": recipient,
            }
            platform = build.HOSTS[sys.platform]
            for ok in (True, False):
                with self.subTest(success=ok):
                    work = root / f"work-{ok}"
                    encrypted = root / f"report-{ok}.age"
                    stdout = io.StringIO()

                    def compile_source(source, output, directory):
                        print("compile diagnostic only")
                        (output / "file").write_bytes(b"synthetic compiled result")
                        if not ok:
                            raise ValueError("compile failed")

                    with patch.object(build, "require_checkout") as require, \
                         patch.object(build, "cpp", side_effect=compile_source), \
                         contextlib.redirect_stdout(stdout):
                        self.assertIs(build.build(argparse.Namespace(
                            request=json.dumps(request), platform=platform, licensed=False,
                            source=root, dotnet=root, work=work, bundle=encrypted,
                        )), ok)
                    require.assert_called_once_with(root.resolve(), request["source_sha"])
                    self.assertNotIn("compile diagnostic", stdout.getvalue())
                    self.assertNotIn(b"synthetic compiled result", encrypted.read_bytes())
                    plain = root / f"report-{ok}.tar"
                    subprocess.run(("age", "-d", "-i", key, "-o", plain, encrypted), check=True)
                    self.assertFalse(work.exists())
                    with tarfile.open(plain) as archive:
                        with archive.extractfile("build.log") as log:
                            self.assertIn(b"compile diagnostic only", log.read())
                        with archive.extractfile("receipt.json") as receipt:
                            identity = json.load(receipt)
                        self.assertEqual(identity["source_sha"], request["source_sha"])
                        self.assertIs(identity["success"], ok)
                        self.assertNotIn("recipient", identity)
                        if ok:
                            with archive.extractfile("payload.tar.gz") as payload:
                                self.assertEqual(
                                    hashlib.file_digest(payload, "sha256").hexdigest(),
                                    identity["payload_sha256"],
                                )
                        else:
                            self.assertNotIn("payload.tar.gz", archive.getnames())


if __name__ == "__main__":
    unittest.main()
