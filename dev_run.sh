PROJECT_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="$PROJECT_ROOT/build"
BINARY="$BUILD_DIR/MeshNetworking"
SSL_CONFIG="$PROJECT_ROOT/certs/openssl.cnf"

export OPENSSL_CONF="$SSL_CONFIG"

echo "--- Building ---"
make build

if [ $? -ne 0 ]; then
    echo "❌ Build failed. Aborting run."
    exit 1
fi

echo "--- Running ---"
"$BINARY" "$@"