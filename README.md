# Fingerprint Double-Tap Fix

*A KernelSU/Magisk module that brings back "double-tap the fingerprint sensor" on HyperOS 4 ports for the Redmi K40S / POCO F4 (`munch`), whose kernel drops the Goodix HAL's double-tap key.*

Module KernelSU/Magisk sửa lỗi tính năng **chạm 2 lần vào cảm biến vân tay cạnh** (Cài đặt → Vân tay → Chạm 2 lần vào cảm biến) không hoạt động trên ROM port HyperOS 4 (Android 17) cho Redmi K40S / POCO F4 (`munch`). ROM đã thử: `OS4.0.24.0.XKKCNXM`, vendor MIUI 14 `V816.0.9.0`, kernel `4.19.325-zyndong-perf`.

## Triệu chứng

Đã chọn hành động (ví dụ mở camera) nhưng chạm 2 lần không có tác dụng gì. Chạm 1 lần (về màn hình chính) vẫn chạy bình thường.

HAL vẫn nhận ra cú chạm 2 lần:
```
[GF_HAL][gf_hal_common]: [gf_hal_common_irq_key] click delta time < g_double_click_timer_msec, send double click event
```
Kernel cũng nhận phím, nhưng không có sự kiện nào tới Android (`getevent` trống):
```
gf_spi: gf_kernel_key_input: received key event[6], key=6, value=1
```

## Nguyên nhân

1. Framework (`PowerFingerprintServiceStubImpl` trong `miui-services.jar`) bật chế độ double-tap bằng `IXiaomiFingerprint.extCmd(12, 2)`. Bước này chạy đúng.
2. HAL Goodix (vendor MIUI 14) gửi cú chạm 2 lần dưới dạng **GF key 6** qua ioctl trên `/dev/goodix_fp`.
3. Driver kernel `gf_spi` chỉ đổi mã cho phím HOME. Mã 6 được chuyển nguyên thành keycode Linux 6. Thiết bị `uinput-goodix` không đăng ký keycode này, nên kernel bỏ sự kiện đi.
4. HyperOS (`MiuiFingerPrintTapListener.shouldInterceptSlideFpTapKey`) lại chờ `KEYCODE_BUTTON_C` (Linux `BTN_C` = 306, theo `/system/usr/keylayout/uinput-goodix.kl`) từ thiết bị có **vendor 0x0666 / product 0x0888**. Thiết bị `uinput-goodix` của kernel này có vendor/product là 0000/0000.

## Cách sửa

`bin/fpdtap` (mã nguồn: [`native/fpdtap.c`](native/fpdtap.c)) là một daemon nhỏ chạy bằng root:

- Tạo thiết bị uinput tên `uinput-goodix`, vendor 0x0666, product 0x0888. Nhờ vậy Android dùng luôn `uinput-goodix.kl` có sẵn, ánh xạ 306 → `BUTTON_C`.
- Đọc `/dev/kmsg`. Mỗi lần nhấn/thả `key=6` được phát lại thành `BTN_C`.
- `service.sh` chạy daemon sau khi máy khởi động xong và tự chạy lại nếu nó dừng.

Không vá file hệ thống nào và không kèm binary của Xiaomi.

## Cài đặt

1. Tải `FpDoubleTapFix-v*.zip` ở mục [Releases](../../releases) rồi cài bằng KernelSU / Magisk.
2. Khởi động lại máy.
3. Chọn hành động ở Cài đặt → Vân tay → Chạm 2 lần vào cảm biến.

Xem log:
```
adb logcat -s FpDoubleTap
```

**Lưu ý:**
- HyperOS cố ý không chạy hành động khi đang ở màn hình khóa.
- Nếu bạn dùng kernel đã sửa sẵn `gf_spi` (đổi key 6 thành `BTN_C` và đặt id 0x0666/0x0888), không cần module này. Cài thêm sẽ khiến phím bị phát hai lần.

## Build

Cần Android NDK (đã thử với r27) và Python 3.

```
aarch64-linux-android30-clang -O2 -Wall -s -o module/bin/fpdtap native/fpdtap.c -llog
python build.py   # -> build/FpDoubleTapFix-<version>.zip
```
