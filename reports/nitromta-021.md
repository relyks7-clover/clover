# NitroMTA 0.2.1 delivery report

- version: nitromta 0.2.1 (release tarball, aarch64-linux)
- os/arch: ubuntu 24.04, aarch64
- install: downloaded the 0.2.1 release archive, extracted into ~/ntm021, ran `./nitromta setup --config config.toml --capture`
- config changes from setup: removed the `[capture]` section; under `[delivery]` set `port = 2525` (kept `require_tls` at its default true)
- identity: account 1, identity `mail.clover.dev` with selector `mail` and a 2048-bit RSA private key (`dkim.pem`), login user `app`
- start: launched as my own user via `setsid ./nitromta run --config config.toml`
- send: smtplib with STARTTLS trusting the generated `./tls.crt`, login `app`, to `proof@nitromta.com`, `Reply-To: tailtalett.0@gmail.com`
- minutes from install to receipt: under 45 (fresh setup to `delivered` at 2026-11-06T03:24:18Z)
- slowest step: diagnosing why NitroMTA 0.2.2 sends were rejected by the proof mailbox with `does not carry NitroMTA's DKIM signature` while the identical 0.2.1 delivery was accepted
- defect / doc gap found: NitroMTA 0.2.2 adds the `X-NitroMTA-Report` header ahead of the `DKIM-Signature` in the delivered message. The proof mailbox (which checks signature form, not DNS) rejects 0.2.2 messages and accepts the same identity/key/message on 0.2.1. Verified: 0.2.2 rejected, 0.2.1 delivered (1 attempt, 0 uncertain). I did not byte-level prove the header order is the exact trigger; it is the only delivered-message format change in 0.2.2 per its changelog, so it is the strongest candidate.
- change i would make: in 0.2.2, either place `X-NitroMTA-Report` after the `DKIM-Signature`, or gate it behind a config flag, and document the ordering impact in the changelog
