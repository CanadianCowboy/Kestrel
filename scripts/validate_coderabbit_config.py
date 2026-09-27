"""Validate .coderabbit.yaml against CodeRabbit's published JSON schema.

Two invalid keys have shipped in this repository and both were silently
ignored, which is the worst possible failure for configuration: it looks like
it is working.

`include_drafts` was never a key -- the setting is `auto_review.drafts` -- so
the draft rule had done nothing since it was written.

`finishing_touches.resolve_merge_conflicts` is not a key either; the schema
spells it in the singular. CodeRabbit caught the second one in review.

Nothing in the toolchain noticed either, because a YAML parser accepts any
key, and CodeRabbit treats an unrecognised setting as absent rather than as an
error. The only way to know a setting is real is to check it against the
schema, so that is what this does.

The schema is vendored next to this script rather than fetched on every run,
so the check is hermetic: a run that fails must mean the configuration is
wrong, not that the network is down. Refresh it deliberately:

    python scripts/validate_coderabbit_config.py --refresh

Other modes:

    python scripts/validate_coderabbit_config.py                 # vendored schema
    python scripts/validate_coderabbit_config.py --online        # fetch the live one
    python scripts/validate_coderabbit_config.py --schema p.json # some other copy

Exits 0 when every key in the configuration is named by the schema, 1 when any
key is not (naming each one), and 2 when the schema or the configuration could
not be read at all.
"""

import argparse
import json
import os
import sys
import urllib.request

try:
    import yaml
except ImportError:  # pragma: no cover - exercised only without PyYAML
    yaml = None

CONFIG = ".coderabbit.yaml"
SCHEMA_URL = "https://coderabbit.ai/integrations/schema.v2.json"
VENDORED = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "coderabbit-schema.v2.json")

# Defined only when PyYAML actually imported: the module sets `yaml` to None
# above when it did not, and subclassing None would turn a missing dependency
# into an ImportError at import time rather than the message main() prints.
if yaml is not None:

    class UniqueKeyLoader(yaml.SafeLoader):
        """A SafeLoader that refuses a mapping that names the same key twice.

        PyYAML's safe_load accepts a repeated key without a word and keeps only
        the last value. Two `finishing_touches:` blocks, or `enabled` twice
        under auto_review, therefore load as one block and this script reports
        that every key is in the schema -- while the half that was dropped is
        doing nothing at all. That is the exact failure this file was written
        to catch, arriving through the one loader it was relying on.
        """

        def construct_mapping(self, node, deep=False):
            seen = set()
            for key_node, _value_node in node.value:
                key = self.construct_object(key_node, deep=deep)
                try:
                    duplicate = key in seen
                except TypeError:
                    # An unhashable key is not a duplicate-key problem, and
                    # failing to hash one is not this loader's to report.
                    continue
                if duplicate:
                    raise yaml.constructor.ConstructorError(
                        "while constructing a mapping", node.start_mark,
                        "found the key %r a second time" % (key,),
                        key_node.start_mark)
                seen.add(key)
            return super().construct_mapping(node, deep=deep)


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def fetch_schema():
    with urllib.request.urlopen(SCHEMA_URL, timeout=30) as response:
        return json.load(response)


def refresh(path):
    """Re-downloads the vendored schema. Only ever written when asked.

    The response is parsed before anything is written, not after. What comes
    back can be an error page, a captive portal, or a body cut off halfway, and
    writing first would leave the vendored schema holding that instead of a
    schema -- after which every later run exits 2 until somebody restores the
    file by hand. The vendored copy is the one thing that makes this check
    hermetic, so corrupting it is the one failure this cannot recover from.
    """
    with urllib.request.urlopen(SCHEMA_URL, timeout=30) as response:
        data = response.read()
    schema = json.loads(data.decode("utf-8"))
    with open(path, "wb") as handle:
        handle.write(data)
    return schema


def child_schema(node, key):
    """The subschema for `key` inside `node`, or None if the schema omits it.

    The keys of a JSON Schema object live under `properties`, not on the node
    itself, so that is where this looks. Reading the node directly finds
    nothing at all and reports the entire configuration as unknown.

    A schema that offers alternatives -- oneOf, anyOf, allOf -- names its keys
    inside the branches instead. Every branch is consulted, and the key counts
    as known if any one of them accepts it, because a union is a statement
    that more than one shape is legal, not a claim that every key is illegal.
    """
    if not isinstance(node, dict):
        return None
    properties = node.get("properties")
    if isinstance(properties, dict) and key in properties:
        return properties[key]
    for combiner in ("oneOf", "anyOf", "allOf"):
        for branch in node.get(combiner, []) or []:
            found = child_schema(branch, key)
            if found is not None:
                return found
    return None


def walk(value, schema, prefix, problems):
    """Reports each key that appears in the config but not in the schema.

    A key the schema does not name is not an error to the YAML parser and not
    an error to CodeRabbit, which reads it as absent. That is the whole reason
    this file exists, so it is also the one thing worth failing on.

    Two shapes need handling that stopping at dicts does not give:

    A list is walked through the schema's `items`, because that is where a JSON
    Schema describes what an entry may contain. path_instructions has nine
    entries in this repository and every one of them is a dict of named keys,
    so without this a typo inside an entry is never looked at -- and an entry
    is exactly where a misspelt key hides, because the key beside it is right.

    A map the schema describes only through `additionalProperties` has no named
    properties at all, and its keys are the data rather than settings:
    mutually_exclusive_groups names its groups by whatever the author calls
    them. Recursion stops there, because there is no list of legal names to
    check them against, and reporting `risk` as an unknown setting would be
    inventing a restriction the schema does not state.
    """
    if isinstance(value, list):
        items = schema.get("items") if isinstance(schema, dict) else None
        if isinstance(items, dict):
            for index, element in enumerate(value):
                walk(element, items, "%s[%d]" % (prefix, index), problems)
        return
    if not isinstance(value, dict):
        return
    if (isinstance(schema, dict) and "properties" not in schema
            and not any(combiners in schema
                        for combiners in ("oneOf", "anyOf", "allOf"))):
        return
    for key, child in value.items():
        path = "%s.%s" % (prefix, key) if prefix else key
        sub = child_schema(schema, key)
        if sub is None:
            problems.append(path)
            continue
        walk(child, sub, path, problems)


def load_schema(args):
    """Returns (schema, error). error is a message already fit to print."""
    if args.refresh:
        try:
            return refresh(args.schema or VENDORED), None
        except Exception as exc:  # noqa: BLE001 - the message is the point
            return None, "could not refresh the CodeRabbit schema: %s" % exc
    if args.online:
        try:
            return fetch_schema(), None
        except Exception as exc:  # noqa: BLE001
            return None, "could not obtain the CodeRabbit schema: %s" % exc
    path = args.schema or VENDORED
    if not os.path.exists(path):
        return None, "no schema at %s; run with --refresh to download one" % path
    try:
        return read_json(path), None
    except Exception as exc:  # noqa: BLE001
        return None, "could not read the schema at %s: %s" % (path, exc)


def main():
    parser = argparse.ArgumentParser(
        description="Check .coderabbit.yaml against CodeRabbit's schema.")
    parser.add_argument("--config", default=CONFIG, help="path to the config file")
    parser.add_argument("--schema", help="validate against this schema file")
    parser.add_argument("--online", action="store_true",
                        help="fetch the live schema instead of the vendored one")
    parser.add_argument("--refresh", action="store_true",
                        help="re-download the vendored schema, then validate")
    args = parser.parse_args()

    if yaml is None:
        print("PyYAML is required to read the configuration.")
        print("    python -m pip install pyyaml")
        return 2

    schema, error = load_schema(args)
    if error is not None:
        print(error)
        return 2

    try:
        with open(args.config, "r", encoding="utf-8") as handle:
            # UniqueKeyLoader, not safe_load: a repeated key is silently
            # dropped by the plain loader, and this is the check that is
            # supposed to notice a setting that quietly stops applying.
            config = yaml.load(handle, Loader=UniqueKeyLoader)
    except Exception as exc:  # noqa: BLE001
        print("could not read %s: %s" % (args.config, exc))
        return 2

    problems = []
    walk(config, schema, "", problems)

    if problems:
        print("%s: %d key(s) are not in the CodeRabbit schema."
              % (args.config, len(problems)))
        print("An unrecognised setting is ignored rather than reported, so each")
        print("of these is doing nothing:")
        for path in sorted(problems):
            print("  - %s" % path)
        return 1

    print("%s: every key is in the CodeRabbit schema" % args.config)
    return 0


if __name__ == "__main__":
    sys.exit(main())
