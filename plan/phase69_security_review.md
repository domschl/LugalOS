# Phase 69 — Security review

**Status: placeholder. Written 2026-11-04. Not scheduled.** To be taken up once the local
infrastructure is upgraded: a proper certificate authority for the nodes (replacing the test CA the
C6-Zero carries since 66.3b), and TLS on the MQTT broker on nalanda.
Milestone scheme: `69.1`, `69.2`, …

---

## 0. Scope

A review of LugalOS's security as built through version 0.19.0: remote administration (phase 52,
ADR 006), TLS (52.14-52.15, 66.3b), isolation (ADR 007, ADR 008), and what the measurements of
phase 66 changed. Not elaborated yet.

## 1. Points collected so far

* **Fuzz the TLS client.** The TLS 1.3 server's parsing has a fuzz harness; the client added in 66.3b
  (`tls_client_init()`, `net/tls.c`, used by MQTT over TLS) has none. A hostile or broken server's
  flight -- ServerHello, EncryptedExtensions with `max_fragment_length`, Certificate,
  CertificateVerify, Finished, and records after the handshake (NewSessionTicket, oversize or
  fragmented records against `TLS_CLIENT_RX_MAX`) -- and the certificate checks behind it
  (`tls_cert_check()`, `kernel/x509.c`: DER, SAN, the CA signature) should be fuzzed on the host
  under ASan/UBSan, like the server's.
