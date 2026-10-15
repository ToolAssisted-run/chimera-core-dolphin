#!/usr/bin/env python3
"""Every default binding names a control the package declares.

chimera#284: the bindings were written as "A" and "Main Stick X" while the
controls are "P1 A" and "P1 Main Stick X". The frontend matches a binding to
a control by its exact name, so not one default ever applied: a gamepad's
stick reached the GameCube's stick only through whatever a person had bound
by hand, and every port but the first had nothing at all.

Checked here, per controller: every name in AllTrollers is a declared button,
every name in AllTrollersAnalog a declared axis, and every axis has a binding.

usage: check-keybinds.py [default_keybinds.json [waterbox.config]]
"""
import json
import sys


def main(binds_path, config_path):
    binds = json.load(open(binds_path))
    config = json.load(open(config_path))
    controllers = {}
    for machine in config.get("machines", [config]):
        inp = machine["input"]
        controllers[inp["name"]] = (set(inp["buttons"]), {a["name"] for a in inp["axes"]})
    wrong = []
    count = 0
    for name, (buttons, axes) in controllers.items():
        digital = binds.get("AllTrollers", {}).get(name, {})
        analog = binds.get("AllTrollersAnalog", {}).get(name, {})
        count += len(digital) + len(analog)
        wrong += [f"{name}: '{b}' is not a button" for b in digital if b not in buttons]
        wrong += [f"{name}: '{a}' is not an axis" for a in analog if a not in axes]
        wrong += [f"{name}: axis '{a}' has no binding" for a in sorted(axes) if a not in analog]
    if wrong:
        print(f"check-keybinds: {len(wrong)} problems:")
        for w in wrong[:20]:
            print(f"  {w}")
        return 1
    print(f"check-keybinds: all {count} default bindings name a declared control, every axis bound")
    return 0


if __name__ == "__main__":
    here = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else here + "/default_keybinds.json",
                  sys.argv[2] if len(sys.argv) > 2 else here + "/waterbox.config"))
