# Bounded repository read bridge for a model host

This directory implements and tests one narrow tool boundary around Suffice 0.13. It does not run Gemma, train a model, select relevant code, edit a repository, or measure issue-resolution performance. No measured Suffice 0.13 source is changed.

## Implemented behavior

The host admits a captured store through the existing native client, then approves a scope using the generation, revision, and row count returned by OPEN. The bridge checks STATE and acquires a PIN with the same native revision. Only host-approved scopes can be read. The host must own the client exclusively and serialize all operations.

The model-facing arguments are scope, generation, first row, row count, and body-byte budget. No arbitrary filesystem root, path, predicate source, cache policy, native alias, publication, or edit operation is accepted. Integers must be actual nonnegative integers; booleans, floats, missing/extra fields, unknown scopes, and excessive ranges/budgets are rejected before dispatch.

Reads use the fixed true predicate through RUNPIN with private collection. The bridge publishes one successful observation only after the existing client has validated completion and the bridge has checked the terminal generation/revision, complete requested range, and retirement policy. It returns sanitized error objects without a collected prefix or native exception text. This stages the model observation; it does not promise transactional external side effects.

Retired generations are rejected by default. A host can explicitly approve historical reads with allow_retired=True; successful observations then disclose retired_generation. A pin preserves the admitted generation, and does not establish that the live worktree remains unchanged. An external edit without recapture/publication will not automatically mark a pin retired. The host owns capture, publication, edit barriers, and any current-worktree check.

Bodies that are valid UTF-8 are represented by an explicitly tagged JSON string. Other bodies use an explicitly tagged base64 string. Decoding either representation recovers the original bytes, including NUL, CR/LF, and arbitrary octets. Body length and a SHA-256 content descriptor accompany every row. The descriptor describes the returned bytes; it is not independent authentication of a hostile native process.

Rendering is canonical and excludes transport, request sequence, timing, and cache diagnostics. Equivalent completed calls through JSONL and Binary therefore produce byte-identical tool-response strings. The host also imposes a rendered-observation byte cap, checked after private collection and JSON construction; it is not a peak-allocation bound. Body bytes, JSON bytes, and model tokens are different quantities; this bridge claims no token reduction.

## Function declaration and dispatch

The checked-in repository_read_schema.json is the default declaration. Hosts with different limits should use bridge.function_schema(), which emits the applicable limits. Give the model only the scopes/generation tokens the host has approved, and dispatch only the declared function name. Pass its parsed argument object to bridge.invoke(); do not evaluate generated code.

Google's current official guidance accepts JSON tool declarations through the processor chat template:

    tool_declarations = [bridge.function_schema()]
    formatted = processor.apply_chat_template(
        messages, tools=tool_declarations,
        tokenize=False, add_generation_prompt=True,
    )

The processor, model, messages, decoding, and tool-response injection are host responsibilities and are not executed by this example. Use the matching model processor and current official format rather than hand-constructing control tokens:

- https://ai.google.dev/gemma/docs/capabilities/text/function-calling-gemma4
- https://ai.google.dev/gemma/docs/core/prompt-formatting-gemma4

This is a standalone callable adapter. The Kaggle prediction harness has its own allowed tools and sandboxed skill route; supplying this function does not automatically make a new top-level tool legal there. A full competition-harness integration remains separate.

## Host setup example

Make the extracted Suffice runtime's scripts directory and this integration directory importable. The following code assumes the host has already created approved_store using the existing capture producer and has selected a native executable:

    from catalog_client import CatalogProcess
    from repository_read_bridge import RepositoryReadBridge

    with CatalogProcess(native_binary, transport="binary") as catalog:
        opened = catalog.open("repository", approved_store, expected_revision=0)
        bridge = RepositoryReadBridge(catalog)
        bridge.bind(
            "task", "repository", "task_pin",
            generation=opened["generation"],
            revision=opened["revision"],
            rows=opened["rows"],
        )
        response = bridge.invoke({
            "scope": "task",
            "generation": opened["generation"],
            "first": 0,
            "count": min(1, opened["rows"]),
            "max_bytes": 262144,
        })

Publication or capture is never delegated to this model-facing function. Use a new generation after a coordinated edit. The class does not expose a shared-client concurrency guarantee, request deadline, whole-process RSS bound, retry protocol, semantic freshness detector, or sandbox.

## Executed validation and reproduction

The final native validation receipt is validation_native/RESULT.json. It records the exact bridge, tests, runtime client, producer, packer, and catalog binary hashes before and after execution. The prior validation_native_initial directory is an earlier development receipt, retained separately.

Run from the paper-revision root, with an extracted Suffice 0.13 release in the adjacent directory:

    python3 integration/test_repository_read_bridge.py \
      --runtime ../Suffice_0_13_0 \
      --binary ../Suffice_0_13_0/build/transport_release/suffice_catalog \
      --packer ../Suffice_0_13_0/build/transport_release/suffice \
      --output integration/validation_new

Use a fresh output directory. These tests require Python zstandard because the real fixture producer creates two captured stores. The bridge/client themselves use the standard library. Use native executables for the platform; Windows has not been executed here.

The test exercises both response transports, empty bodies, arbitrary bytes, Unicode paths/text, bounded ranges, wrong scope/generation, unknown native pins, native and rendered output budgets, host-approval mismatches, retirement rejection, and explicitly approved historical reads. It then injects a failure after one nonempty row from the actual native process and verifies that no successful or partial model observation is returned. A forged terminal generation also closes the client. These are correctness checks, not independent timing trials.
