# shared settings for the host test scripts (sourced)
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
T=${PKR_TEST_DIR:-/tmp/pkr}
HOSTBIN=${HOSTBIN:-$REPO/build/port-host-sapphire/pokemon_host}
SAV=$REPO/tools/port/test/test.sav
mkdir -p "$T"
