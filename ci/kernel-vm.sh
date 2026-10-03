#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
work=$(mktemp -d /tmp/isotab-kernel-vm.XXXXXX)
vm_pid=
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then tail -100 "$work/serial.log" 2>/dev/null || true; fi
    if [ -n "$vm_pid" ]; then kill "$vm_pid" 2>/dev/null || true; wait "$vm_pid" 2>/dev/null || true; fi
    rm -rf -- "$work"
    exit "$status"
}
trap cleanup EXIT

# Canonical's Jammy KVM image uses the GA 5.15 kernel. Fetch the checksum
# alongside the image over HTTPS and reject both corruption and kernel drift.
base=https://cloud-images.ubuntu.com/releases/jammy/release
image=ubuntu-22.04-server-cloudimg-amd64-disk-kvm.img
curl --fail --location --retry 3 --max-time 300 "$base/SHA256SUMS" -o "$work/SHA256SUMS"
curl --fail --location --retry 3 --max-time 600 "$base/$image" -o "$work/$image"
(cd "$work"; awk -v image="$image" '$2 == "*" image || $2 == image' SHA256SUMS > image.sha256; test -s image.sha256; sha256sum -c image.sha256)
qemu-img resize "$work/$image" 10G
ssh-keygen -q -t ed25519 -N '' -f "$work/key"
cat > "$work/user-data" <<EOF
#cloud-config
users:
  - name: tester
    shell: /bin/bash
    sudo: ['ALL=(ALL) NOPASSWD:ALL']
    ssh_authorized_keys:
      - $(cat "$work/key.pub")
ssh_pwauth: false
disable_root: true
EOF
printf '%s\n' 'instance-id: isotab-kernel-test' 'local-hostname: isotab-kernel-test' > "$work/meta-data"
cloud-localds "$work/seed.img" "$work/user-data" "$work/meta-data"
accel=tcg
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then accel=kvm; fi
qemu-system-x86_64 -accel "$accel" -m 2048 -smp 2 -display none \
    -serial "file:$work/serial.log" -monitor none \
    -drive "file=$work/$image,format=qcow2,if=virtio" \
    -drive "file=$work/seed.img,format=raw,if=virtio" \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=net0 &
vm_pid=$!
ssh_options=(-i "$work/key" -p 2222 -o BatchMode=yes -o ConnectTimeout=3
    -o StrictHostKeyChecking=accept-new -o "UserKnownHostsFile=$work/known_hosts")
ready=false
for attempt in $(seq 1 120); do
    kill -0 "$vm_pid"
    if ssh "${ssh_options[@]}" tester@127.0.0.1 true 2>/dev/null; then ready=true; break; fi
    sleep 2
done
"$ready"
git archive --format=tar HEAD > "$work/source.tar"
ssh "${ssh_options[@]}" tester@127.0.0.1 'cat > source.tar' < "$work/source.tar"
timeout 1200 ssh "${ssh_options[@]}" tester@127.0.0.1 bash -se <<'GUEST'
set -euxo pipefail
uname -a
case "$(uname -r)" in 5.15.*) ;; *) echo 'Expected a real Linux 5.15 kernel'; exit 1;; esac
sudo cloud-init status --wait
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y gcc make pkg-config libgtk-3-dev libglib2.0-dev librsvg2-common xvfb xauth
mkdir source
tar -xf source.tar -C source
cd source
pkg-config --modversion gtk+-3.0 glib-2.0
make
make test
NO_AT_BRIDGE=1 xvfb-run -a make ui-test
GUEST
