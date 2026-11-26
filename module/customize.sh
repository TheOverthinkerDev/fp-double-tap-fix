SKIPUNZIP=0
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/fpdtap" 0 0 0755
mkdir -p /data/adb/fpdtap
ui_print "- fpdtap sẽ chạy sau khi khởi động lại"
ui_print "- Double-tap: chọn hành động ở Cài đặt > Vân tay"
ui_print "- Camera: vuốt lên/xuống trên cảm biến để zoom"
ui_print "  Tắt vuốt-zoom: touch /data/adb/fpdtap/swipe_zoom_off"
ui_print "  Số nấc 0.1x mỗi lần vuốt: echo 5 > /data/adb/fpdtap/zoom_steps"
ui_print "  Đảo chiều: touch /data/adb/fpdtap/swipe_invert"
