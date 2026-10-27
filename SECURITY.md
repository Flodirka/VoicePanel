# Security

VoicePanel is designed for local use. The default API address is `127.0.0.1:8080`. It includes endpoints that process files, synthesize voices and manage backend processes.

Keep the service and inference engines on loopback for normal panel use. Do not forward their ports to the internet. An optional service bearer token is available for API clients; the desktop panel does not currently provide a token setting. CORS configuration is not an authentication mechanism.

Recordings, voice references, transcripts, context documents and exported Markdown can contain private information. Keep them out of issues and pull requests. Check debug output before sharing it. The sample configuration disables transcript logging by default.

## Reporting a vulnerability

Use this repository's **Security → Report a vulnerability** page for a private report. Include reproduction steps, the affected component and potential impact. Use synthetic audio and text. Do not publish exploit details or credentials in a public issue.

## Boundary and limits

Host, Origin and optional token checks cover HTTP and WebSocket requests. Request bodies are bounded while they are received. The standard launcher requires authentication before binding to a specific non-loopback interface and rejects wildcard binds. It does not provide TLS; direct internet exposure is unsupported.

Loopback is not a boundary against malware already running under the same user account. Local voice references, transcripts and preferences are ordinary local files; VoicePanel does not encrypt them. Keep the operating-system account and runtime dependencies trusted. Do not run the panel as administrator.

Runtime downloads use pinned URLs and SHA-256 values, verify cached files and downloaded bytes, and reject unsafe archive entries. Runtime Python dependencies have an artifact-hashed lock. Matching a checksum does not prove that an upstream program is free of vulnerabilities.

## Repository changes

The default branch requires a pull request, passing repository checks and resolved review conversations. Only the maintainer has write access. Contributions are not merged or published automatically. CODEOWNERS requests maintainer review. External workflow runs require maintainer approval; CI uses a read-only token, pinned actions and no production secrets. Source changes, dependency and download updates still require human review.

Security fixes target the current default branch. There is no guaranteed response time or long-term support policy.
