"""Child-process supervision for inference backends.

Rules enforced here (from the specification):

* The service only ever terminates processes it started itself.
* If a healthy server already listens on the configured port, it is *adopted*
  instead of being replaced; adoption requires the health probe to answer.
* Idle processes are stopped after a configurable TTL once all leases drop.
* A crash while leases are held triggers a bounded restart with backoff.
"""
from __future__ import annotations

import asyncio
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Awaitable, Callable

import httpx

CREATE_NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0


class ProcessError(RuntimeError):
    pass


class ManagedProcess:
    """One supervised inference server child process."""

    def __init__(
        self,
        *,
        name: str,
        command: list[str],
        host: str,
        port: int,
        health_path: str = "/health",
        health_check: Callable[[httpx.Response], bool] | None = None,
        start_timeout_s: float = 120.0,
        idle_stop_s: float = 1800.0,
        cwd: str | None = None,
        extra_env: dict[str, str] | None = None,
        prepend_path: list[str] | None = None,
        log_dir: Path | None = None,
        spawn: bool = True,
    ) -> None:
        self.name = name
        self.command = command
        self.host = host
        self.port = port
        self.health_path = health_path
        self.health_check = health_check or (lambda resp: resp.status_code == 200)
        self.start_timeout_s = start_timeout_s
        self.idle_stop_s = idle_stop_s
        self.cwd = cwd
        self.extra_env = extra_env or {}
        self.prepend_path = prepend_path or []
        self.log_dir = log_dir
        # False: the server is someone else's (an agent's own llama.cpp, for
        # instance). Use it when it answers, never start or stop it.
        self.spawn = spawn

        self._proc: subprocess.Popen | None = None
        self._adopted = False
        self._lock = asyncio.Lock()          # serialises start/stop transitions
        self._leases = 0
        self._last_used = time.monotonic()
        self._started_at: float | None = None
        self._stop_requested = False
        self._restarts = 0
        self._idle_task: asyncio.Task | None = None

    # ------------------------------------------------------------------ state
    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    @property
    def running(self) -> bool:
        if self._adopted:
            return True
        return self._proc is not None and self._proc.poll() is None

    @property
    def pid(self) -> int | None:
        if self._adopted:
            return None
        return self._proc.pid if self._proc else None

    def status(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "running": self.running,
            "adopted": self._adopted,
            "pid": self.pid,
            "leases": self._leases,
            "restarts": self._restarts,
            "uptime_s": round(time.monotonic() - self._started_at, 1) if self._started_at and self.running else None,
            "idle_s": round(time.monotonic() - self._last_used, 1) if self.running and self._leases == 0 else 0.0,
            "base_url": self.base_url,
        }

    # ------------------------------------------------------------- lifecycle
    async def start(self) -> None:
        """Ensure the backend is reachable; starts or adopts as needed."""
        async with self._lock:
            if not self.spawn:
                # Someone else's server: adopt it while it answers, never replace it.
                if await self._probe(timeout=min(self.start_timeout_s, 5.0)):
                    if not self.running:
                        self._adopted = True
                        self._started_at = time.monotonic()
                    return
                raise ProcessError(
                    f"{self.name}: no server answering at {self.base_url}; "
                    f"start it, or remove 'external' to let the service run its own"
                )
            if await self._probe():
                if not self.running:
                    if await self._reclaim_owned_orphan():
                        await self._spawn()
                        return
                    self._adopted = True   # healthy server already on the port; keep it
                    self._started_at = time.monotonic()
                return
            if self._adopted and not await self._probe():
                # adopted server went away; fall through and start our own
                self._adopted = False
            if self.running:
                # process alive but not answering; give it a moment then fail loudly
                if not await self._probe(timeout=self.start_timeout_s):
                    raise ProcessError(f"{self.name}: process running but health check failed on {self.base_url}")
                return
            await self._spawn()

    def _pid_file(self) -> Path | None:
        if self.log_dir:
            state = self.log_dir.parent / "state"
            return state / f"{self.name}.pid"
        return None

    def _write_pid(self) -> None:
        pid_file = self._pid_file()
        if pid_file and self._proc:
            pid_file.parent.mkdir(parents=True, exist_ok=True)
            pid_file.write_text(str(self._proc.pid), encoding="utf-8")

    async def _reclaim_owned_orphan(self) -> bool:
        """A previous service instance may have left our server running with a
        stale environment/config. Reclaim (stop) it when the recorded PID points
        to a process started with our own command line; never touch others."""
        pid_file = self._pid_file()
        if not pid_file or not pid_file.is_file():
            return False
        try:
            pid = int(pid_file.read_text(encoding="utf-8").strip())
        except ValueError:
            pid_file.unlink(missing_ok=True)
            return False
        try:
            import psutil

            proc = psutil.Process(pid)
            cmdline = " ".join(proc.cmdline()).lower()
            marker = str(self.command[0]).lower()
            if marker not in cmdline:
                return False  # not ours; leave it alone and adopt instead
            for child in proc.children(recursive=False):
                child.terminate()
            proc.terminate()
            try:
                await asyncio.wait_for(asyncio.to_thread(proc.wait), timeout=10)
            except asyncio.TimeoutError:
                proc.kill()
        except Exception:
            pass  # process already gone or not inspectable
        pid_file.unlink(missing_ok=True)
        return True

    async def _spawn(self) -> None:
        log_file = None
        if self.log_dir:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            log_file = open(self.log_dir / f"{self.name}.log", "ab", buffering=0)
        env = dict(os.environ)
        for k, v in self.extra_env.items():
            env[k] = v
        if self.prepend_path:
            env["PATH"] = os.pathsep.join(self.prepend_path) + os.pathsep + env.get("PATH", "")
        creationflags = CREATE_NO_WINDOW
        try:
            self._proc = subprocess.Popen(
                self.command,
                cwd=self.cwd,
                env=env,
                stdout=log_file if log_file else subprocess.DEVNULL,
                stderr=subprocess.STDOUT if log_file else subprocess.DEVNULL,
                creationflags=creationflags,
            )
        except FileNotFoundError as exc:
            raise ProcessError(f"{self.name}: binary not found: {self.command[0]}") from exc
        finally:
            if log_file:
                log_file.close()  # the child keeps its own duplicated handle
        self._started_at = time.monotonic()
        self._stop_requested = False
        try:
            await self._probe(timeout=self.start_timeout_s)
        except Exception:
            await self._kill_owned()
            raise
        self._write_pid()
        self._restarts += 1
        self._ensure_idle_watcher()

    async def _probe(self, timeout: float = 0.0) -> bool:
        deadline = time.monotonic() + max(timeout, 0.1)
        async with httpx.AsyncClient(timeout=2.0, trust_env=False) as client:
            while True:
                try:
                    resp = await client.get(self.base_url + self.health_path)
                    if resp.status_code == 200 and self.health_check(resp):
                        return True
                except httpx.HTTPError:
                    pass
                if time.monotonic() >= deadline:
                    return False
                await asyncio.sleep(0.5)

    async def stop(self) -> None:
        """Stop the process if we own it. Never touches adopted/external servers."""
        async with self._lock:
            await self._kill_owned()

    async def _kill_owned(self) -> None:
        self._adopted = False
        proc, self._proc = self._proc, None
        if proc is None:
            return
        if proc.poll() is None:
            proc.terminate()
            try:
                await asyncio.wait_for(asyncio.to_thread(proc.wait), timeout=10)
            except asyncio.TimeoutError:
                proc.kill()
                try:
                    await asyncio.wait_for(asyncio.to_thread(proc.wait), timeout=5)
                except asyncio.TimeoutError:  # pragma: no cover
                    pass
        self._started_at = None

    # ---------------------------------------------------------------- leases
    def lease(self) -> "Lease":
        """Context manager: ensures the process is up for the duration of a request."""
        return Lease(self)

    def _release(self) -> None:
        self._leases = max(0, self._leases - 1)
        self._last_used = time.monotonic()

    def _ensure_idle_watcher(self) -> None:
        if self._idle_task is None or self._idle_task.done():
            self._idle_task = asyncio.create_task(self._idle_watch())

    async def _idle_watch(self) -> None:
        while self.running:
            await asyncio.sleep(15)
            if (
                self.idle_stop_s > 0
                and self._leases == 0
                and not self._adopted
                and time.monotonic() - self._last_used > self.idle_stop_s
            ):
                await self.stop()
                return


class Lease:
    """Keeps a backend process alive for the duration of a request."""

    def __init__(self, proc: ManagedProcess) -> None:
        self._proc = proc

    async def __aenter__(self) -> Lease:
        await self._proc.start()
        self._proc._leases += 1
        self._proc._last_used = time.monotonic()
        return self

    async def __aexit__(self, *exc: Any) -> None:
        self._proc._release()

    def release(self) -> None:
        self._proc._release()
