from typing import Dict, Any, Callable

class GlobalRegistry:
    """
    Service Registry for the Vesta ecosystem.
    Allows discovery and invocation of services across the kernel.
    """
    _services: Dict[str, Any] = {}

    @classmethod
    def register(cls, name: str, service: Any):
        cls._services[name] = service

    @classmethod
    def get(cls, name: str) -> Any:
        if name not in cls._services:
            raise KeyError(f"Service {name} not found in Global Registry")
        return cls._services[name]

    @classmethod
    def call(cls, service_name: str, method_name: str, *args, **kwargs):
        service = cls.get(service_name)
        method = getattr(service, method_name)
        return method(*args, **kwargs)
