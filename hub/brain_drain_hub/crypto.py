from cryptography.fernet import Fernet


def encrypt(key: str, plain: str) -> str:
    return Fernet(key.encode()).encrypt(plain.encode()).decode()


def decrypt(key: str, token: str) -> str:
    return Fernet(key.encode()).decrypt(token.encode()).decode()
