# Localhost TLS test fixtures

Commit `localhost.crt` and `localhost.key` together. The WebSocket tests use
them for a local TLS server and explicitly trust this certificate in the test
client. CMake supplies this directory to the tests, so CI needs no TLS secrets
or certificate-generation step.

This key is intentionally public and only for tests. Never reuse it for a real
service. The live Coinbase client uses the system trust store instead.
