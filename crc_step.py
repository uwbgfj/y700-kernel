
import struct
syms = ['i2c_register_driver', 'i2c_del_driver', 'device_property_present', 'of_count_phandle_with_args', '__of_parse_phandle_with_args', 'of_drm_find_panel', 'of_property_read_string', 'strlen', 'strnstr', 'devm_kmalloc', 'snprintf', 'of_find_property', 'of_property_read_variable_u32_array', '__init_swait_queue_head', 'device_property_read_u32_array', 'devm_gpiod_get_optional', 'gpiod_direction_input', 'gpiod_direction_output', 'devm_request_threaded_irq', 'disable_irq', 'gpiod_set_value', 'msleep', 'gpiod_to_irq', 'sysfs_create_group', '_printk', '_dev_err', '_dev_info', 'input_unregister_device', '__stack_chk_fail', 'panel_event_notifier_unregister', 'regulator_disable', 'sysfs_remove_group', 'kfree', 'devm_pinctrl_get', 'pinctrl_lookup_state', 'regulator_get', '_dev_warn', 'regulator_put', 'regulator_enable', 'usleep_range_state', 'wait_for_completion_interruptible_timeout', 'complete', 'i2c_transfer', 'input_mt_report_pointer_emulation', 'input_event', 'pinctrl_select_state', 'desc_to_gpio', 'kmalloc_caches', 'kmalloc_trace', 'krealloc', '__kmalloc', 'request_firmware_nowait', 'panel_event_notifier_register', 'regulator_count_voltages', 'regulator_set_load', 'regulator_set_voltage', 'input_mt_report_slot_state', 'i2c_transfer_buffer_flags', 'irq_get_irq_data', 'enable_irq', 'kmemdup_nul', 'sscanf', 'input_allocate_device', 'input_set_capability', 'input_set_abs_params', 'input_alloc_absinfo', 'input_mt_init_slots', 'input_register_device', 'release_firmware', 'input_free_device', 'memcpy', 'fortify_panic', 'scnprintf', 'request_firmware', 'mutex_lock', 'mutex_unlock', 'module_layout']
d = open('vmlinux','rb').read()
e_shoff=struct.unpack_from('<Q',d,0x28)[0]; es=struct.unpack_from('<H',d,0x3a)[0]
n=struct.unpack_from('<H',d,0x3c)[0]; si=struct.unpack_from('<H',d,0x3e)[0]
def sh(i): return e_shoff+i*es
so=struct.unpack_from('<Q',d,sh(si)+0x18)[0]
def nm(i):
    o=struct.unpack_from('<I',d,sh(i))[0]; e=d.index(b'\0',so+o); return d[so+o:e].decode()
sec={}
for i in range(n):
    sec[nm(i)]=(struct.unpack_from('<Q',d,sh(i)+0x18)[0], struct.unpack_from('<Q',d,sh(i)+0x20)[0])
def load(kt, kc):
    out={}
    if kt not in sec or kc not in sec: return out
    to,ts=sec[kt]; co,cs=sec[kc]
    for i in range(ts//12):
        base=to+i*12
        name_off=struct.unpack_from('<i',d,base+4)[0]
        na=base+4+name_off
        e=d.index(b'\0',na); name=d[na:e].decode(errors='replace')
        if i*4+4<=cs:
            out[name]=struct.unpack_from('<I',d,co+i*4)[0]
    return out
m={}
m.update(load('__ksymtab','__kcrctab'))
m.update(load('__ksymtab_gpl','__kcrctab_gpl'))
print('===CRC_BEGIN===')
for s in syms:
    print(s, ('%08x'%m[s]) if s in m else 'MISSING')
print('===CRC_END===')
