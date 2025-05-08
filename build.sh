#!/bin/bash

set -euo pipefail

echo "▶ Starting compilation and installation of the Phantom kernel 🛠️"

# Ensure required tools are installed
REQUIRED_TOOLS=("base-devel" "clang" "llvm" "make" "bc" "ncurses" "flex" "bison" "libelf" "git" "sudo")
for TOOL in "${REQUIRED_TOOLS[@]}"; do
    if ! pacman -Qq "$TOOL" &> /dev/null; then
        echo "❌ Missing required tool: $TOOL. Please install it using 'pacman -S' and rerun the script."
        exit 1
    fi
done

# Clear previous build
echo "🧹 Cleaning previous build..."
make clean

sleep 1

# Check for .config
if [[ ! -f .config ]]; then
    echo "❌ No .config found. Please create or copy a kernel configuration file."
    exit 1
fi

# Set local kernel version suffix
echo "🔧 Setting LOCALVERSION to -phantom"
./scripts/config --file .config --set-str LOCALVERSION "-phantom"

# Enable Clang LTO
./scripts/config --enable LTO_CLANG
./scripts/config --enable LTO_CLANG_THIN

# Start compiling kernel
echo "🚀 Compiling kernel with Clang + ThinLTO..."
for CPU in /sys/devices/system/cpu/cpu[0-9]*; do
    if [[ -f "$CPU/cpufreq/scaling_governor" ]]; then
        echo performance | sudo tee "$CPU/cpufreq/scaling_governor" > /dev/null
    fi
done

make LLVM=1 \
     CC=clang \
     CXX=clang++ \
     HOSTCC=clang \
     LD=ld.lld \
     AR=llvm-ar \
     NM=llvm-nm \
     OBJCOPY=llvm-objcopy \
     OBJDUMP=llvm-objdump \
     STRIP=llvm-strip \
     -j$(nproc) | tee build.log 

# Install modules
echo "📦 Installing modules..."
sudo make modules_install \
          LLVM=1 \
          CC=clang \
          CXX=clang++ \
          HOSTCC=clang \
          LD=ld.lld \
          AR=llvm-ar \
          NM=llvm-nm \
          OBJCOPY=llvm-objcopy \
          OBJDUMP=llvm-objdump \
          STRIP=llvm-strip \
          -j$(nproc) | tee -a build.log

# Export kernel artifacts
echo "📤 Exporting kernel artifacts..."
KERNEL_VERSION=-phantom
sudo cp ./arch/x86/boot/bzImage "/boot/vmlinuz${KERNEL_VERSION}"
sudo cp System.map "/boot/System.map${KERNEL_VERSION}"
sudo cp .config "/boot/config${KERNEL_VERSION}"

# Generate initramfs and update GRUB
echo "📝 Generating initramfs and updating GRUB..."
sudo mkinitcpio -p linux-phantom | tee -a build.log
sudo grub-mkconfig -o /boot/grub/grub.cfg | tee -a build.log

# Reset CPU scaling governor
for CPU in /sys/devices/system/cpu/cpu[0-9]*; do
    if [[ -f "$CPU/cpufreq/scaling_governor" ]]; then
        echo schedutil | sudo tee "$CPU/cpufreq/scaling_governor" > /dev/null
    fi
done

echo "✅ Phantom kernel has been installed. Reboot to apply the changes. 😋"

# Define optimization flags
# export KBUILD_CFLAGS="-O2 -march=znver2 -mtune=znver2 -pipe -flto=thin -fomit-frame-pointer -ffunction-sections -fdata-sections -funroll-loops -falign-functions=32 -falign-loops=8 -falign-jumps=8 -fbranch-target-load-optimize -fbranch-target-load-optimize2"
# export KBUILD_LDFLAGS="-fuse-ld=lld -Wl,--as-needed -Wl,--gc-sections -Wl,--icf=all -Wl,--sort-common -z relro -z now -z pack-relative-relocs"
