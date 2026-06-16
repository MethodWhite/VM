import sys
from .registry.registry import GlobalRegistry
from .bus.bus import SharedStateBus

class VestaCLI:
    """
    Command-dispatcher for the 'vesta' single binary entrypoint.
    """
    def __init__(self):
        self.commands = {
            "compile": self._compile,
            "run": self._run,
            "zenith": self._zenith,
            "debug": self._debug
        }

    def dispatch(self, args):
        if len(args) < 2:
            print("Usage: vesta <command> [args...]")
            print("Commands: compile, run, zenith, debug")
            sys.exit(1)

        cmd = args[1]
        if cmd not in self.commands:
            print(f"Unknown command: {cmd}")
            sys.exit(1)

        self.commands[cmd](args[2:])

    def _compile(self, args):
        print("Vesta-Kernel: Compiling source to Vex bytecode...")
        # Implementation for Vex compiler integration

    def _run(self, args):
        print("Vesta-Kernel: Bootstrapping Vex runtime and executing...")
        # Implementation for VM execution

    def _zenith(self, args):
        print("Vesta-Kernel: Launching Zenith Reflection Engine...")
        # Implementation for Zenith launch

    def _debug(self, args):
        print("Vesta-Kernel: Entering Debug mode...")
        # Implementation for debugger

def main():
    cli = VestaCLI()
    cli.dispatch(sys.argv)
