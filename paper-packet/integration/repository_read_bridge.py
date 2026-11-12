"""A bounded host-approved repository read tool; no model inference or editing."""
from __future__ import annotations

import base64
from dataclasses import dataclass
import hashlib
import json
import re

from catalog_client import CatalogError

ALL_ROWS = "const yes 1\noutput yes > 0\n"
SCHEMA = "suffice.repository_observation.v1"
_LABEL = re.compile(r"[A-Za-z][A-Za-z0-9_.-]{0,63}\Z")
_GENERATION = re.compile(r"[0-9a-f]{32}\Z")
_UINT64_MAX = (1 << 64) - 1


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _uint(value, maximum):
    return type(value) is int and 0 <= value <= maximum


def _error(code):
    # Never include native exception text, paths, or a partially collected body.
    return _json({"schema": SCHEMA, "status": "error", "error": {"code": code}})


@dataclass(frozen=True)
class _Scope:
    alias: str
    generation: str
    revision: int
    rows: int
    allow_retired: bool


class RepositoryReadBridge:
    """Own the client exclusively and serialize all host and model operations.

    The host captures/publishes data and approves scopes. This object offers no
    filesystem traversal, arbitrary query source, edits, or current-tree proof.
    """

    def __init__(self, client, *, max_rows=16, max_bytes=262144,
                 max_observation_bytes=2097152):
        if (not _uint(max_rows, 1024) or not max_rows
                or not _uint(max_bytes, 16 << 20)
                or not _uint(max_observation_bytes, 128 << 20)
                or max_observation_bytes < 1024):
            raise ValueError("invalid host limits")
        self._client = client
        self.max_rows = max_rows
        self.max_bytes = max_bytes
        self.max_observation_bytes = max_observation_bytes
        self._scopes = {}
        self._aliases = set()

    def bind(self, scope, project, alias, *, generation, revision, rows,
             allow_retired=False):
        """Host-only approval using generation/revision/rows from successful OPEN."""
        if (not all(type(x) is str and _LABEL.fullmatch(x)
                    for x in (scope, project, alias))
                or type(generation) is not str or not _GENERATION.fullmatch(generation)
                or not _uint(revision, _UINT64_MAX) or not revision
                or not _uint(rows, _UINT64_MAX) or type(allow_retired) is not bool):
            raise ValueError("invalid host binding")
        if scope in self._scopes or alias in self._aliases:
            raise ValueError("scope or alias already bound")
        state = self._client.state(project)
        if (state.get("loaded") is not True
                or state.get("generation") != generation
                or type(state.get("revision")) is not int
                or state["revision"] != revision
                or type(state.get("rows")) is not int or state["rows"] != rows):
            raise ValueError("host approval does not match admitted project")
        pinned = self._client.pin(alias, project)
        if type(pinned.get("revision")) is not int or pinned["revision"] != revision:
            self._client.unpin(alias)
            raise ValueError("pin revision differs from host approval")
        self._scopes[scope] = _Scope(alias, generation, revision, rows, allow_retired)
        self._aliases.add(alias)

    def function_schema(self):
        """JSON tool declaration for a Gemma-compatible host; no model is loaded."""
        return {
            "type": "function",
            "function": {
                "name": "read_repository",
                "description": (
                    "Read complete artifact rows from a host-approved pinned "
                    "repository generation. A pin is not a claim of current "
                    "worktree freshness. No filesystem path or edit is accepted."
                ),
                "parameters": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "scope": {"type": "string", "minLength": 1, "maxLength": 64,
                                  "description": "Host-provided scope identifier."},
                        "generation": {"type": "string", "pattern": "^[0-9a-f]{32}$",
                                       "description": "Exact host-provided generation token."},
                        "first": {"type": "integer", "minimum": 0, "maximum": _UINT64_MAX,
                                  "description": "First generation-local row index."},
                        "count": {"type": "integer", "minimum": 0, "maximum": self.max_rows,
                                  "description": "Number of complete rows, not text lines."},
                        "max_bytes": {"type": "integer", "minimum": 0, "maximum": self.max_bytes,
                                      "description": "Body-byte budget, not a model-token budget."},
                    },
                    "required": ["scope", "generation", "first", "count", "max_bytes"],
                },
            },
        }

    def invoke(self, arguments):
        """Validate a parsed model argument object before dispatch."""
        if (type(arguments) is not dict
                or set(arguments) != {"scope", "generation", "first", "count", "max_bytes"}):
            return _error("invalid_arguments")
        return self.read_repository(**arguments)

    def _fatal(self, code):
        try:
            self._client.close(force=True)
        except Exception:
            pass
        return _error(code)

    def read_repository(self, scope: str, generation: str, first: int,
                        count: int, max_bytes: int) -> str:
        """Read approved complete rows and return one canonical JSON observation.

        Args:
            scope: Host-provided scope identifier.
            generation: Exact generation approved for this scope.
            first: First generation-local input row, not a line number.
            count: Number of complete input rows.
            max_bytes: Maximum total body bytes.
        """
        if (type(scope) is not str or not _LABEL.fullmatch(scope)
                or type(generation) is not str or not _GENERATION.fullmatch(generation)
                or not _uint(first, _UINT64_MAX)
                or not _uint(count, self.max_rows)
                or not _uint(max_bytes, self.max_bytes)):
            return _error("invalid_arguments")
        binding = self._scopes.get(scope)
        if binding is None:
            return _error("unknown_scope")
        if generation != binding.generation:
            return _error("generation_mismatch")
        if first > binding.rows or count > binding.rows - first:
            return _error("invalid_range")
        try:
            # Private collection: no callback or model-visible output before the
            # client's validated terminal. Cache is not populated by this tool.
            result, rows = self._client.run(
                binding.alias, ALL_ROWS, first=first, count=count, cache="bypass",
                emit="hex", max_rows=count, max_bytes=max_bytes, pinned=True,
                collect=True,
            )
        except CatalogError:
            return _error("native_rejected")
        except Exception:
            return self._fatal("delivery_failed")
        if (result.get("generation") != binding.generation
                or type(result.get("revision")) is not int
                or result["revision"] != binding.revision
                or type(result.get("retired_generation")) is not bool
                or result.get("op") != "RUNPIN" or result.get("status") != "ok"):
            return self._fatal("identity_mismatch")
        if result["retired_generation"] and not binding.allow_retired:
            return _error("retired_generation")
        # ALL_ROWS is a fixed true predicate; this tool promises exactly count
        # rows, rather than accepting a silently shortened range.
        if (len(rows) != count or result.get("selected_rows") != count
                or [r["row"] for r in rows] != list(range(first, first + count))
                or result.get("output_bytes") != sum(len(r["data"]) for r in rows)):
            return self._fatal("incomplete_observation")
        rendered_rows = []
        for row in rows:
            body = row["data"]
            try:
                content = {"encoding": "utf8", "text": body.decode("utf-8", "strict")}
            except UnicodeDecodeError:
                content = {"encoding": "base64",
                           "data": base64.b64encode(body).decode("ascii")}
            rendered_rows.append({
                "row": row["row"], "path": row["path"], "bytes": len(body),
                "sha256": hashlib.sha256(body).hexdigest(), "content": content,
            })
        observation = {
            "schema": SCHEMA, "status": "ok", "scope": scope,
            "generation": binding.generation, "revision": binding.revision,
            "consistency": "pinned_generation",
            "retired_generation": result["retired_generation"],
            "range": {"first": first, "count": count},
            "row_count": len(rows), "body_bytes": sum(len(r["data"]) for r in rows),
            "rows": rendered_rows,
        }
        wire = _json(observation)
        if len(wire) > self.max_observation_bytes:  # ensure_ascii=True => ASCII bytes.
            return _error("observation_budget_exceeded")
        return wire
