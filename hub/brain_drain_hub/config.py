import os
from dataclasses import dataclass


@dataclass
class Settings:
    db_path: str
    secret_key: str            # Fernet key (urlsafe base64, 32 bytes)
    admin_password: str
    poll_interval: float = 60  # seconds; 0 disables the background poller
    public_url: str = ""       # if set, added to each device's webhook list on add

    @classmethod
    def from_env(cls) -> "Settings":
        for var in ("HUB_SECRET_KEY", "HUB_ADMIN_PASSWORD"):
            if not os.environ.get(var):
                raise SystemExit(f"{var} must be set (see README)")
        return cls(
            db_path=os.environ.get("HUB_DB", "hub.db"),
            secret_key=os.environ["HUB_SECRET_KEY"],
            admin_password=os.environ["HUB_ADMIN_PASSWORD"],
            poll_interval=float(os.environ.get("HUB_POLL_INTERVAL", "60")),
            public_url=os.environ.get("HUB_PUBLIC_URL", "").rstrip("/"),
        )
