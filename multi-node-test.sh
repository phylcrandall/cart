#!/bin/bash
# Phyl -- with my changes
# The original is in DAOS_INFO/CART/brians-multi-node-test.sh

set -ex
# Phyl
#set -ex -o pipefail

# Phyl
# A list of tests to run as a single instance on Jenkins
JENKINS_TEST_LIST=(scripts/cart_echo_test.yml                   \
                   scripts/cart_echo_test_non_sep.yml           \
                   scripts/cart_test_group.yml                  \
                   scripts/cart_test_group_non_sep.yml          \
                   scripts/cart_test_barrier.yml                \
                   scripts/cart_test_barrier_non_sep.yml        \
                   scripts/cart_threaded_test.yml               \
                   scripts/cart_threaded_test_non_sep.yml       \
                   scripts/cart_test_rpc_error.yml              \
                   scripts/cart_test_rpc_error_non_sep.yml      \
                   scripts/cart_test_singleton.yml              \
                   scripts/cart_test_singleton_non_sep.yml      \
                   scripts/cart_rpc_test.yml                    \
                   scripts/cart_rpc_test_non_sep.yml            \
                   scripts/cart_test_corpc_version.yml          \
                   scripts/cart_test_corpc_version_non_sep.yml  \
                   scripts/cart_test_cart_ctl.yml               \
                   scripts/cart_test_cart_ctl_non_sep.yml       \
                   scripts/cart_test_iv.yml                     \
                   scripts/cart_test_iv_non_sep.yml             \
                   scripts/cart_test_proto.yml                  \
                   scripts/cart_test_proto_non_sep.yml          \
                   scripts/cart_test_no_timeout.yml             \
                   scripts/cart_test_no_timeout_non_sep.yml)

# shellcheck disable=SC1091
if [ -f .localenv ]; then
    # read (i.e. environment, etc.) overrides
    . .localenv
fi

HOSTPREFIX=${HOSTPREFIX-${HOSTNAME%%.*}}
NFS_SERVER=${NFS_SERVER:-$HOSTPREFIX}

trap 'echo "encountered an unchecked return code, exiting with error"' ERR

# shellcheck disable=SC1091
. .build_vars-Linux.sh

# shellcheck disable=SC2154
# Phyl -- so this gets rid of any leftover mount points from a previous run
# I moved this up from where it was in the original
# Phyl
echo "SL_OMPI_PREFIX = ${SL_OMPI_PREFIX}"

# Phyl DAOS_BASE is /home/cart/cart or the /var/lib equiv.
DAOS_BASE=${SL_OMPI_PREFIX%/install/*}
echo "DAOS_BASE = ${DAOS_BASE}"

echo "HOSTNAME=${HOSTNAME}"

EXECUTOR_NUMBER=${EXECUTOR_NUMBER:-2}
echo "EXECUTOR_NUMBER = ${EXECUTOR_NUMBER}"

# Brian said to use this instead of hard-coding vms 11 and 12 11/30/2018
#first_vm=$((EXECUTOR_NUMBER+4)*2-1)
first_vm=$(($(($((EXECUTOR_NUMBER+4))*2))-1))
echo $first_vm
second_vm=$(($((EXECUTOR_NUMBER+4))*2))
echo $second_vm
vm1=vm"$(((EXECUTOR_NUMBER+4)*2-1))"
vm2=vm"$(((EXECUTOR_NUMBER+4)*2))"
#vm1=vm$first_vm
echo $vm1
echo $vm2


trap 'set +e
i=5
# due to flakiness on wolf-53, try this several times
while [ $i -gt 0 ]; do
    pdsh -R ssh -S -w ${HOSTPREFIX}vm[1-12] "set -x
    x=0
    while [ \$x -lt 30 ] &&
          grep $DAOS_BASE /proc/mounts &&
          ! sudo umount $DAOS_BASE; do
        ps axf
        sleep 1
        let x+=1
    done
    sudo rmdir $DAOS_BASE || find $DAOS_BASE || true" 2>&1 | dshbak -c
    if [ ${PIPESTATUS[0]} = 0 ]; then
        i=0
    fi
    let i-=1
done' EXIT

# Phyl -- I moved this up.
#DAOS_BASE=${SL_OMPI_PREFIX%/install/*}
# Phyl -- the following edits the /etc/fstab file
# Need to change 11-12 to something like $vm1-$vm2
#if ! pdsh -R ssh -S -w "${HOSTPREFIX}"vm[${first_vm}-${second_vm}] "set -ex
if ! pdsh -R ssh -S -w "${HOSTPREFIX}"vm[1-12] "set -ex
ulimit -c unlimited
sudo mkdir -p $DAOS_BASE
sudo ed <<EOF /etc/fstab
\\\$a
$NFS_SERVER:$PWD $DAOS_BASE nfs defaults 0 0 # added by ftest.sh
.
wq
EOF
sudo mount $DAOS_BASE

# TODO: package this in to an RPM
pip3 install --user tabulate

df -h" 2>&1 | dshbak -c; then
    echo "Cluster setup (i.e. provisioning) failed"
    exit 1
fi

echo "hit enter to continue"
#read -r
#exit 0

# Phyl -- create the config file
if [ "$1" = "2" ]; then
    cat <<EOF > install/Linux/TESTING/scripts/config.json
{
    "host_list": ["${HOSTPREFIX}${vm1}", "${HOSTPREFIX}${vm2}"],
    "use_daemon":"DvmRunner"
}
EOF
fi

rm -rf install/Linux/TESTING/testLogs/

# shellcheck disable=SC2029
if ! ssh "${HOSTPREFIX}"vm1 "set -ex
ulimit -c unlimited
cd $DAOS_BASE

# now run it!
pushd install/Linux/TESTING
if [ \"$1\" = \"2\" ]; then
    python3 test_runner config=scripts/config.json \\
        "${JENKINS_TEST_LIST[@]}" || {
        rc=\${PIPESTATUS[0]}
        echo \"Test exited with \$rc\"
    }
fi
exit \$rc"; then
    rc=${PIPESTATUS[0]}
else
    rc=0
fi

{
    cat <<EOF
TestGroup:
    submission: $(TZ=UTC date)
    test_group: CART_${1}-node
    testhost: $HOSTNAME
    user_name: jenkins
Tests:
EOF
    find install/Linux/TESTING/testLogs -name subtest_results.yml -print0 | \
         xargs -0 cat
} > results_1.yml

exit "$rc"