/**
 * @file    preset_config.h
 * @brief   预置位 YAML 配置文件读写
 *
 *          文件路径: ./config/preset_positions.yaml
 *
 *          YAML 格式:
 *            groups:
 *              - groupNum: 1
 *                points:
 *                  - index: 0
 *                    horAngle: 0
 *                    verAngle: 0
 *                    success: true
 *                  - index: 1
 *                    horAngle: 45
 *                    verAngle: 10
 *                    success: false
 *              - groupNum: 2
 *                points: []
 *
 *          组1 对应 Pelco-D 预置位 1~16，组2 对应 33~48。
 */

#ifndef PRESET_CONFIG_H
#define PRESET_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   将预置位组数据写入 YAML 文件
 * @param   config_dir  配置目录路径，如 "./config"
 * @return  0 成功，-1 失败
 *
 *          从 PTZ 模块内存读取两组预置位并写入 preset_positions.yaml。
 *          原子写入：先写 .tmp 再 rename。
 */
int preset_config_save(const char *config_dir);

/**
 * @brief   从 YAML 文件读取预置位数据并恢复到 PTZ 模块内存
 * @param   config_dir  配置目录路径，如 "./config"
 * @return  0 至少加载了一组，-1 文件不存在或格式错误
 *
 *          如果文件不存在，返回 -1（调用者当作无历史预置位）。
 *          读取成功后调用 device_ptz_load_preset_group() 逐组恢复。
 */
int preset_config_load(const char *config_dir);

/**
 * @brief   删除 preset_positions.yaml 文件
 * @param   config_dir  配置目录路径，如 "./config"
 *
 *          用于预置位设置（cmd=103）前与硬件同步清除。
 *          文件不存在时静默跳过（errno == ENOENT）。
 */
void preset_config_clear(const char *config_dir);

#ifdef __cplusplus
}
#endif

#endif /* PRESET_CONFIG_H */
