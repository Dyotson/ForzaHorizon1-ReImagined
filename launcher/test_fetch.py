"""TLS context selection and the certificate error message (#85), without the network."""
import ssl
import sys
import unittest
import urllib.error
from pathlib import Path
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import fetch, updater


def cert_error():
    reason = ssl.SSLCertVerificationError(1, "[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: "
                                             "unable to get local issuer certificate")
    return urllib.error.URLError(reason)


class ContextSelectionTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(setattr, fetch, "_CONTEXTS", fetch._CONTEXTS)
        fetch._CONTEXTS = None

    def test_windows_and_macos_try_the_os_store_then_the_default_context(self):
        for platform in ("win32", "darwin"):
            fetch._CONTEXTS = None
            os_ctx, default = MagicMock(name="truststore"), MagicMock(name="default")
            with self.subTest(platform=platform), patch.object(fetch.sys, "platform", platform), \
                    patch.object(fetch, "_truststore_context", return_value=os_ctx), \
                    patch.object(fetch, "_default_context", return_value=default):
                self.assertEqual(fetch.contexts(), [os_ctx, default])
                self.assertIs(fetch.ssl_context(), os_ctx)

    def test_windows_without_truststore_uses_the_default_context(self):
        default = MagicMock(name="default")
        with patch.object(fetch.sys, "platform", "win32"), \
                patch.object(fetch, "_truststore_context", return_value=None), \
                patch.object(fetch, "_default_context", return_value=default):
            self.assertEqual(fetch.contexts(), [default])

    def test_linux_keeps_the_default_context(self):
        default = MagicMock(name="default")
        with patch.object(fetch.sys, "platform", "linux"), \
                patch.object(fetch, "_truststore_context") as ts, \
                patch.object(fetch, "_default_context", return_value=default):
            self.assertEqual(fetch.contexts(), [default])
            ts.assert_not_called()

    def test_truststore_import_failure_gives_none(self):
        with patch.dict(sys.modules, {"truststore": None}):
            self.assertIsNone(fetch._truststore_context())

    def test_contexts_verify(self):
        for ctx in fetch.contexts():
            self.assertEqual(ctx.verify_mode, ssl.CERT_REQUIRED)
            self.assertTrue(ctx.check_hostname)


class UrlopenTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(setattr, fetch, "_CONTEXTS", fetch._CONTEXTS)
        self.first, self.second = object(), object()
        fetch._CONTEXTS = [self.first, self.second]

    def test_falls_back_to_the_next_context_on_a_certificate_error(self):
        calls = []

        def fake(req, timeout, context):
            calls.append(context)
            if context is self.first:
                raise cert_error()
            return "response"
        with patch.object(fetch.urllib.request, "urlopen", side_effect=fake), \
                patch("forza_launcher.paths.log_error") as log:
            self.assertEqual(fetch.urlopen("https://github.com/x", timeout=5), "response")
        self.assertEqual(calls, [self.first, self.second])
        log.assert_called_once()

    def test_every_context_failing_gives_an_actionable_error_with_the_host(self):
        with patch.object(fetch.urllib.request, "urlopen", side_effect=cert_error()), \
                patch("forza_launcher.paths.log_error") as log:
            with self.assertRaises(fetch.CertificateError) as cm:
                fetch.urlopen("https://github.com/x/y.zip")
        text = str(cm.exception)
        self.assertIn("github.com", text)
        self.assertRegex(text, "antivirus")
        self.assertRegex(text, "proxy")
        self.assertIn("launcher.log", text)
        self.assertEqual(log.call_count, 2)

    def test_other_errors_are_not_retried(self):
        error = urllib.error.URLError(ConnectionRefusedError())
        with patch.object(fetch.urllib.request, "urlopen", side_effect=error) as op:
            with self.assertRaises(urllib.error.URLError):
                fetch.urlopen("https://github.com/x")
        self.assertEqual(op.call_count, 1)
        http = urllib.error.HTTPError("https://github.com/x", 404, "nf", {}, None)
        with patch.object(fetch.urllib.request, "urlopen", side_effect=http) as op:
            with self.assertRaises(urllib.error.HTTPError):
                fetch.urlopen("https://github.com/x")
        self.assertEqual(op.call_count, 1)
        http.close()

    def test_update_check_reports_the_certificate_problem(self):
        with patch.object(fetch.urllib.request, "urlopen", side_effect=cert_error()), \
                patch("forza_launcher.paths.log_error"):
            with self.assertRaisesRegex(updater.UpdateError, "api.github.com.*antivirus"):
                updater.check()


if __name__ == "__main__":
    unittest.main()
