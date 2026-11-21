# API reference

The default gateway address is `http://127.0.0.1:8080`. Requests use HTTP and JSON, except audio uploads and the WebSocket dictation stream. Selected routes use OpenAI-compatible request shapes; this is not a complete OpenAI implementation.

## Request security

HTTP and WebSocket requests share Host, browser-origin and optional token checks. Local Host names are `127.0.0.1`, `localhost` and `::1`; an explicitly configured fixed bind address is also accepted. Browser origins are denied unless listed in `service.allow_origins`. There is no wildcard origin bypass.

If `service.auth_token` or `VOICE_SERVICE_AUTH_TOKEN` is set, send `Authorization: Bearer <token>` or `X-API-Key: <token>`. Do not put credentials in URLs. Native WebSocket clients use the same headers. The desktop panel does not currently have a bearer-token setting.

`service.max_upload_mb` limits the complete HTTP request body, including multipart overhead. The default is 200 MiB. WebSocket messages are limited to at most 1 MiB. The normal launcher requires authentication for a non-loopback address and rejects wildcard binds. These checks do not provide TLS or make the service suitable for direct internet exposure. See [Security](../SECURITY.md).

## Health and discovery

- `GET /health`: gateway liveness, version and uptime. It does not load inference models.
- `GET /v1/models`: configured logical model IDs and their capabilities. TTS entries include `engine` and `speech_parameters`: `speed`, `instructions`, `voice_cloning`, `voice_design` and `options`. `speech_aliases.auto` provides the descriptor when the default names one model and has no language-specific override.
- `GET /v1/capabilities`: available speech and text capabilities, compute choices and process state.
- `GET /v1/text/modes`: supported text modes and context profiles.

## Transcription

`POST /v1/audio/transcriptions` accepts multipart form data. `file` is required. Common optional fields are `model`, `language`, `prompt` and `response_format`.

`model` selects a configured ASR model; omission uses the service default. `language` can be `auto`, `en`, `ru` or another language supported by the selected engine. `prompt` supplies recognition context. `response_format` accepts `json`, `text` or `verbose_json`. The default JSON result has a `text` field. Verbose output adds available language, duration, segments, backend, model and processing-time data.

```sh
curl --fail -X POST http://127.0.0.1:8080/v1/audio/transcriptions \
  -F file=@example.wav \
  -F language=auto \
  -F response_format=json
```

Use a synthetic or explicitly approved local recording. Nothing in the repository supplies a personal reference recording.

## Speech synthesis

`POST /v1/audio/speech` accepts JSON:

```json
{"input":"Hello. This is a local speech example.","model":"kokoro","voice":"af_heart","response_format":"wav"}
```

`input` is required. `model` selects a configured speech model. A known voice-profile ID in `voice` uses its available cloning model, unless an explicit cloning-capable model overrides it. Profiles from a disabled engine use the configured cloning model. `language` and `speed` are optional. Native OmniVoice accepts neutral `speed: 1` without forwarding it; other speed values return 400. `response_format` defaults to `wav`; `pcm` returns PCM16 samples and an `X-Sample-Rate` header.

`instructions` accepts comma-separated voice attributes from the selected model's `speech_parameters.voice_design`. For example:

```json
{"input":"Hello. This is a designed voice.","model":"omnivoice-q8","instructions":"female, young adult, low pitch","options":{"seed":42}}
```

Omit `voice` for voice design. A profile and design instructions in the same request return 400. Native OmniVoice can also synthesize an automatic voice without a profile. Supported options depend on the engine: native HTTP OmniVoice accepts `seed` (32 generation steps fixed upstream); the legacy CLI accepts `steps` and `seed`; audio.cpp OmniVoice accepts `num_inference_steps`, `guidance_scale` and `seed`; Qwen3-TTS accepts `max_tokens`. Piper and Kokoro reject these design options. Unsupported fields return 400 before calling the engine. Use discovery metadata instead of sending one parameter set to every model.

For agent applications, configure `tts.default_voices.en` and `tts.default_voices.ru` with profile IDs from `GET /v1/voices` or built-in model IDs such as `piper-ru-female`, then restart the service. When both `voice` and a specific `model` are omitted, the configured language default is used. An explicit `voice`, specific `model` or design instructions override it. An empty language default uses `tts.default`, with language routing when it is `auto`. A deleted configured profile returns 503. Clients can send `{"input":"Привет"}` to use the Russian default or `{"input":"Hello","voice":"my-profile"}` to override it.

The Russian defaults are the small Piper voices (male `piper-ru`, female `piper-ru-female`, both real-time on CPU); a cloned profile is selected explicitly and synthesizes through the OmniVoice engine.

Cloning requires a configured model and a reference profile. Use only recordings you have permission to use. Generating speech does not send a voice message.

## Voice profiles

Profiles remain in local runtime storage. The API returns profile metadata and reference transcripts; keep access restricted to trusted local clients.

| Route | Request and result |
|---|---|
| `GET /v1/voices` | List profiles in a `voices` array |
| `POST /v1/voices` | Multipart `name` and `file`; optional `reference_text`, `language`, `transcribe`; return the created `voice` |
| `GET /v1/voices/{id}` | Return one `voice`, or `404` |
| `PATCH /v1/voices/{id}` | Optional multipart `name`, `reference_text`, `language`, `model`, `file`; return the updated `voice` |
| `DELETE /v1/voices/{id}` | Delete that local profile and return its ID in `deleted` |
| `POST /v1/voices/{id}/preview` | Return sample WAV audio and `X-Generation-Ms` |

Reference duration must be between 2 and 60 seconds. Profiles use generated safe IDs. A profile includes its name, model, language, reference text, reference duration, timestamps and WAV-presence state. The reference WAV is not returned by these metadata routes.

## Text processing

`POST /v1/text/process` accepts:

```json
{"text":"This is a sentence.","mode":"correct_en","profile":"general"}
```

`text` and `mode` are required. Text is limited to 32,000 characters. Modes are `dictation`, `correct_en`, `correct_ru`, `translate_ru_en` and `translate_en_ru`. Profiles are `general`, `ai_prompts`, `programming`, `godot`, `unity` and `unreal`.

Optional fields are `dictionary` (string list), `context` (string), `previous` (string), `spelling` (objects with `word` and `suggestions`), `temperature` (default `0.2`), `max_tokens` (integer or null), and `explain` (default `false`). `profile` defaults to `general`.

The response includes `mode`, `profile`, `original`, `result` and `model`. Model-backed results also include processing duration and available usage data. Explanation requests can include `notes`. Plain `dictation` returns the text without calling an LLM.

Before the model runs, a deterministic pre-pass fixes near-miss renderings of `dictionary` words in the transcript (ratio >= 0.75, two-token splits glued, inflections left alone) and applies `spelling` hints that agree with the dictionary; every fix is reported in `notes` as `heard -> fixed`. With `llm.skip_if_clean` (default `true`), a `correct_*` request whose pre-pass found nothing and that carries no `spelling` flags returns the text unchanged with `model: null` and `notes: "clean"`, without calling the model. Set `skip_if_clean: false` to always run the model.

Translation modes (`translate_ru_en`, `translate_en_ru`) use the optional `backends.llama_translate` backend when it is configured (a small specialist such as Hy-MT2-1.8B); correction modes and `dictation` always use `backends.llama`.

When `max_tokens` is omitted or null, the service does not set an output-token cap. Generation uses the model's end-of-response token and the backend's context window. An explicit `max_tokens` is forwarded unchanged. If a backend reports an incomplete response without an explicit client cap, the service keeps the original and reports the failure:

```json
{"detail":"the text model returned an incomplete response; the original text was kept","reason":"incomplete_generation","original":"..."}
```

The status is `502`, `result` is absent, and `original` returns the submitted text. A client must not treat `original` as a correction or translation. An explicit `max_tokens` keeps the previous behavior: the response is `200` with whatever the model generated.

The default `backends.llama.template_style: loqira_gec` uses the [GEC model's trained system prompt](https://huggingface.co/loqira/Qwen3.5-0.8B-GEC-KAZ-RUS-ENG), thinking off and independent correction requests. It applies the terminology/spelling pre-pass, then sends the current text without extra system instructions or previous turns. For a general instruction model, set `template_style: ""` to use the generic profile/context prompt. PLaMo translation keeps its separate native template. Continuous microphone capture does not require a new model conversation for each phrase.

In the panel, a failed batch stays queued. The next finalized phrase retries it together with the new text, in order, without restarting capture. The configured silence rollover archives the transcript and resets processing while capture stays active.

## Session warm-up

`POST /v1/warmup` prepares the models a dictation session is about to use and answers `202` at once:

```json
{"asr":true,"llm":true,"mode":"auto_correct","profile":"general","context":"..."}
```

All fields are optional. `mode` accepts the text modes above plus `auto_correct` and `auto_translate`; `dictation` or no mode skips the text model. The service loads the configured ASR model if it is not resident, and sends a one-token request with the session's system prompt to the local text server, so the first sentence reuses that prompt prefix. The response lists the scheduled parts in `scheduled`. Nothing runs for a resident ASR model, for a warm-up already in progress, or for an external text server (`backends.llama.external: true`), whose slots belong to another application. The panel calls this route when a dictation session starts; older services answer `404`, which the panel ignores.

## Chat completions

`POST /v1/chat/completions` forwards the JSON request to the configured text server. The gateway maps `model` to its served model alias. `stream: true` returns server-sent events. An external text server receives the corresponding text and context.

## WebSocket dictation

Connect to `ws://127.0.0.1:8080/v1/sessions/dictation`. Send PCM16 little-endian, mono, 16 kHz audio as binary messages. The server rechunks it into 512-sample VAD frames.

JSON controls are:

- `{"type":"config","model":"nemotron","language":"auto"}`; optional `speech_threshold` and `silence_frames` update segmentation.
- `{"type":"pause"}` and `{"type":"resume"}` pause or resume intake.
- `{"type":"finish"}` finalizes remaining audio and ends the session.
- `{"type":"cancel"}` discards pending session state and ends it.

Events include `ready`, `config_applied`, `speech_started`, `partial`, `final`, `error` and `done`. Transcript events contain `text` and an elapsed `t_offset_s`. Audio intake continues while an ended utterance is recognized; `final` events keep utterance order, and `finish` sends `done` after the last `final`. A failed recognition sends an `error` event and the session continues. With `sessions.speculative: true`, `partial` events start once an utterance has `min_speech_frames` speech frames. The server-side stream is separate from the Qt panel's capture/controller implementation. It does not transmit voice messages to another application.

## Administration

These routes manage local processes and settings. They are not public deployment endpoints.

- `GET /admin/status`: gateway and backend process state, including the `omnivoice` cloning engine.
- `POST /admin/backends/{name}/load`, `/unload`, `/restart`: manage `audiocpp`, `audiocpp_clone`, `audiocpp_diar`, native HTTP `omnivoice`, `whisper` or `llama` when configured. `clone` follows the selected cloning process; the legacy OmniVoice CLI starts through speech requests.
- `GET /admin/compute`: current and available device choices for `main`, `clone`, `diar` and `llm`.
- `POST /admin/compute`: apply a body such as `{"devices":{"main":"cpu","llm":"cpu"}}`. Allowed values are `cpu` and `gpu`, subject to installed runtime support. Busy or externally managed processes cause `409`. `clone` follows the selected model's process; `llm` switches correction and translation together.
- `GET /admin/engines`: active and pending TTS/ASR choices, installed alternatives and `restart_required`.
- `POST /admin/engines`: save next-start choices, for example `{"tts":"omnivoice","asr":"audiocpp"}`. TTS accepts `omnivoice` or `audiocpp`; ASR accepts `audiocpp` or `whisper`. Missing binaries or weights return 400. This does not download files or restart the service.
- `POST /admin/shutdown`: stop owned inference processes and exit the gateway.

## Speaker diarization

`POST /v1/audio/diarize` accepts a WAV upload and returns the speaker turns
computed by the `nemotron-diar` model:

```json
{"model":"nemotron-diar","speaker_turns":[{"start_sample":10560,"end_sample":17760,"speaker_id":"speaker_0","confidence":0.97}]}
```

Offsets are samples at the file's rate. The meeting view in the panel uses
this endpoint to label remote speakers; other clients can call it directly.

The meeting window keeps two tabs — free-form **Notes** and the live
**Transcript** — and on stop saves one Markdown document named after the
meeting's full start time (`Meeting 2026-11-06 00-54-33.md`) into the
configured folder (default `Documents\VoicePanel\Meetings`): the Notes
section first, the Transcript section second. Auto-save is a setting in
the meeting's popover.

## Errors

Typical codes are `400` for invalid input or a forbidden Host, `401` for failed authentication, `403` for a blocked browser origin, `404` for an unknown model/profile, `409` for a busy compute change, `413` for an oversized request, `502` for an inference error, and `504` for an unreachable backend.

Route errors use `detail`; security-boundary errors use an `error` object. Rejected WebSocket handshakes use policy code `1008`; an oversized accepted message closes with `1009`.
