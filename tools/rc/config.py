"""遥控台共享配置，不依赖桌面 GUI。"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .joystick import AxisRoles
from .mapping import ManualControls, MappingConfig
from .protocol import FlapLimits

CONFIG_PATH = Path.home() / ".bfcopilot" / "rc.json"


@dataclass
class AppConfig:
    host: str = ""
    port: int = 3335
    mapping: dict = field(default_factory=dict)
    manual: dict = field(default_factory=dict)
    limits: dict = field(default_factory=dict)
    roles: dict = field(default_factory=dict)


def load_config() -> AppConfig:
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return AppConfig()
    if not isinstance(data, dict):
        return AppConfig()
    config = AppConfig(host=str(data.get("host", "")))
    try:
        config.port = int(data.get("port", 3335))
    except (TypeError, ValueError):
        pass
    for name in ("mapping", "manual", "limits", "roles"):
        if isinstance(data.get(name), dict):
            setattr(config, name, data[name])
    return config


def save_config(config: AppConfig) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(asdict(config), ensure_ascii=False, indent=2),
                           encoding="utf-8")


def mapping_from_dict(data: dict) -> MappingConfig:
    return _from_dict(MappingConfig, data)


def manual_from_dict(data: dict) -> ManualControls:
    return _from_dict(ManualControls, data)


def limits_from_dict(data: dict) -> FlapLimits:
    values = asdict(FlapLimits())
    values.update({key: value for key, value in data.items() if key in values})
    for key in ("min_deg", "max_deg", "speed_dps", "acceleration_dps2"):
        sequence = list(values[key]) + [0.0, 0.0]
        values[key] = tuple(float(value) for value in sequence[:2])
    return FlapLimits(**values)


def roles_from_dict(data: dict) -> AxisRoles:
    values = asdict(AxisRoles())
    values.update({key: value for key, value in data.items() if key in values})
    for key in ("arm_button", "estop_button"):
        value = values[key]
        values[key] = None if value in (None, "", "无") else int(value)
    return AxisRoles(**values)


def _from_dict(model, data: dict):
    values = asdict(model())
    values.update({key: value for key, value in data.items() if key in values})
    return model(**values)
