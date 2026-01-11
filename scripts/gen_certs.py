import os
import sys
import datetime
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

PROJECT_ROOT: str = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CERTS_DIR: str = os.path.join(PROJECT_ROOT, "certs")
CA_KEY_FILE: str = os.path.join(CERTS_DIR, "ca_key.pem")
CA_CERT_FILE: str = os.path.join(CERTS_DIR, "ca_cert.pem")

def generate_key() -> rsa.RSAPrivateKey:
    """Generates a new RSA 2048 private key."""
    return rsa.generate_private_key(
        public_exponent=65537,
        key_size=2048,
    )

def save_key(key: rsa.RSAPrivateKey, filename: str) -> None:
    """Saves a private key to disk in PEM format (unencrypted)."""
    with open(filename, "wb") as f:
        f.write(key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        ))

def save_cert(cert: x509.Certificate, filename: str) -> None:
    """Saves a certificate to disk in PEM format."""
    with open(filename, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))

def ensure_ca() -> None:
    """
    Checks if the Root CA exists. If not, generates a new private key
    and a self-signed Root Certificate.
    """
    if os.path.exists(CA_KEY_FILE) and os.path.exists(CA_CERT_FILE):
        return

    print(f"[+] Generating Root CA in {CERTS_DIR}...")
    os.makedirs(CERTS_DIR, exist_ok=True)

    # generate the CA key
    ca_key: rsa.RSAPrivateKey = generate_key()
    save_key(ca_key, CA_KEY_FILE)

    # these CA certificates are self signed
    subject: x509.Name = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, u"US"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, u"MeshNetwork Root"),
        x509.NameAttribute(NameOID.COMMON_NAME, u"MeshNetwork CA"),
    ])
    
    now = datetime.datetime.now(datetime.timezone.utc)
    
    ca_cert: x509.Certificate = x509.CertificateBuilder().subject_name(
        subject
    ).issuer_name(
        subject 
    ).public_key(
        ca_key.public_key()
    ).serial_number(
        x509.random_serial_number()
    ).not_valid_before(
        now
    ).not_valid_after(
        now + datetime.timedelta(days=3650)
    ).add_extension(
        x509.BasicConstraints(ca=True, path_length=None), critical=True,
    ).sign(ca_key, hashes.SHA256())

    save_cert(ca_cert, CA_CERT_FILE)
    print("[+] CA Created.")

def create_node_cert(node_name: str) -> None:
    """
    Generates a private key and a certificate for a specific node,
    signed by the Root CA.
    """
    node_dir: str = os.path.join(CERTS_DIR, node_name)
    if os.path.exists(node_dir):
        print(f"[-] Certificates for '{node_name}' already exist.")
        return

    print(f"[+] Creating certificates for node: {node_name}")
    os.makedirs(node_dir, exist_ok=True)

    with open(CA_KEY_FILE, "rb") as f:
        ca_key = serialization.load_pem_private_key(f.read(), password=None)
        if not isinstance(ca_key, rsa.RSAPrivateKey):
            raise TypeError("CA Key is not an RSA Private Key")

    with open(CA_CERT_FILE, "rb") as f:
        ca_cert: x509.Certificate = x509.load_pem_x509_certificate(f.read())

    # generate node key
    node_key: rsa.RSAPrivateKey = generate_key()
    save_key(node_key, os.path.join(node_dir, "node_key.pem"))

    # create the CSR
    csr: x509.CertificateSigningRequest = x509.CertificateSigningRequestBuilder().subject_name(x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, u"US"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, u"MeshNetwork Node"),
        x509.NameAttribute(NameOID.COMMON_NAME, f"{node_name}"),
    ])).add_extension(
        x509.SubjectAlternativeName([x509.DNSName(f"{node_name}")]),
        critical=False,
    ).sign(node_key, hashes.SHA256())

    # sign the CSR with the CA
    now = datetime.datetime.now(datetime.timezone.utc)
    
    node_cert: x509.Certificate = x509.CertificateBuilder().subject_name(
        csr.subject
    ).issuer_name(
        ca_cert.subject
    ).public_key(
        csr.public_key()
    ).serial_number(
        x509.random_serial_number()
    ).not_valid_before(
        now
    ).not_valid_after(
        now + datetime.timedelta(days=365)
    ).add_extension(
        x509.BasicConstraints(ca=False, path_length=None), critical=True,
    ).sign(ca_key, hashes.SHA256())

    save_cert(node_cert, os.path.join(node_dir, "node_cert.pem"))
    
    # copy the generated CA root so the node can verify other nodes
    with open(CA_CERT_FILE, "rb") as src:
        with open(os.path.join(node_dir, "ca_root.pem"), "wb") as dst:
            dst.write(src.read())

    print(f"[+] Success. Files located in: certs/{node_name}/")

if __name__ == "__main__":
    ensure_ca()
    if len(sys.argv) < 2:
        print("Usage: python scripts/gen_certs.py <node_name>")
    else:
        create_node_cert(sys.argv[1])