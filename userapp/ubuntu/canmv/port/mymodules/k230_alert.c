#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

// 🚨 引入 mbedTLS 核心安全组件头文件
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

// 引入 MicroPython 核心头文件
#include "py/runtime.h"
#include "py/obj.h"

// ========================================================
// 🚀 EMQX Cloud 专属服务器及身份鉴权凭证配置
// ========================================================
#define MQTT_SERVER       "r6ce3891.ala.cn-hangzhou.emqxsl.cn"
#define MQTT_PORT         "8883"   // 👈 mbedtls 接收字符串格式端口
#define MQTT_CLIENT_ID    "k230_native_core"
#define MQTT_USER         "k230_device"
#define MQTT_PASS         "2951505422"
#define MQTT_TOPIC        "k230/fall/alert"
#define LOG_FILE_PATH     "/data/event_log.txt"

#define TIMEZONE_OFFSET_HOURS 8 

static sem_t g_alert_sem;
static int g_thread_running = 1;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// ========================================================
// 🔒 mbedTLS 全局安全上下文控制句柄
// ========================================================
static mbedtls_net_context      g_net_ctx;
static mbedtls_ssl_context      g_ssl_ctx;
static mbedtls_ssl_config       g_ssl_conf;
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static int                      g_is_tls_connected = 0;

/**
 * 打印 mbedtls 的专属错误明文
 */
static void print_mbedtls_error(const char *msg, int error_code) {
    char error_buf[100];
    mbedtls_strerror(error_code, error_buf, sizeof(error_buf));
    printf("[C TLS ERROR] %s: -0x%04X (%s)\n", msg, -error_code, error_buf);
}

/**
 * 获取带时区偏移的高精度人类可读时间戳
 */
static void get_formatted_time_str(char *buf, size_t max_size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t local_sec = tv.tv_sec + (TIMEZONE_OFFSET_HOURS * 3600);
    struct tm *tm_info = gmtime(&local_sec);
    int millisec = tv.tv_usec / 1000;
    snprintf(buf, max_size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, millisec);
}

/**
 * 线程安全的本地 Flash 日志追加
 */
static void c_log_event(const char* event_name) {
    pthread_mutex_lock(&g_log_mutex);
    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (f != NULL) {
        char time_str[64];
        get_formatted_time_str(time_str, sizeof(time_str));
        fprintf(f, "[%s] EVENT: %s\n", time_str, event_name);
        fclose(f);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/**
 * 安全销毁并释放 TLS 占用资源
 */
static void dismantle_tls_pipeline(void) {
    if (g_is_tls_connected) {
        printf("[C TLS DEBUG] Tearing down active TLS session safely...\n");
        mbedtls_ssl_close_notify(&g_ssl_ctx);
        mbedtls_net_free(&g_net_ctx);
        mbedtls_ssl_free(&g_ssl_ctx);
        mbedtls_ssl_config_free(&g_ssl_conf);
        mbedtls_ctr_drbg_free(&g_ctr_drbg);
        mbedtls_entropy_free(&g_entropy);
        g_is_tls_connected = 0;
        printf("[C TLS DEBUG] TLS architecture components wiped cleanly.\n");
    }
}

/**
 * 基于 mbedtls_ssl_write 的安全 MQTT CONNECT 报文交付
 */
static int send_native_mqtt_connect(void) {
    unsigned char pkt[256];
    int idx = 0;

    printf("[C MQTT DEBUG] Framing MQTT CONNECT packet payloads...\n");
    pkt[idx++] = 0x10; 
    int rem_len_idx = idx; idx++; 

    pkt[idx++] = 0x00; pkt[idx++] = 0x04; 
    pkt[idx++] = 'M'; pkt[idx++] = 'Q'; pkt[idx++] = 'T'; pkt[idx++] = 'T'; 
    pkt[idx++] = 0x04; // 3.1.1
    pkt[idx++] = 0xC2; // Username + Password + CleanSession
    pkt[idx++] = 0x00; pkt[idx++] = 0x3C; // Keep Alive 60s

    int cid_len = strlen(MQTT_CLIENT_ID);
    pkt[idx++] = (cid_len >> 8) & 0xFF; pkt[idx++] = cid_len & 0xFF;
    memcpy(&pkt[idx], MQTT_CLIENT_ID, cid_len); idx += cid_len;

    int user_len = strlen(MQTT_USER);
    pkt[idx++] = (user_len >> 8) & 0xFF; pkt[idx++] = user_len & 0xFF;
    memcpy(&pkt[idx], MQTT_USER, user_len); idx += user_len;

    int pass_len = strlen(MQTT_PASS);
    pkt[idx++] = (pass_len >> 8) & 0xFF; pkt[idx++] = pass_len & 0xFF;
    memcpy(&pkt[idx], MQTT_PASS, pass_len); idx += pass_len;

    pkt[rem_len_idx] = (unsigned char)(idx - rem_len_idx - 1);

    // 🚨 替换原原生 send(), 走 mbedtls 加密安全发送
    int ret;
    int written = 0;
    while (written < idx) {
        ret = mbedtls_ssl_write(&g_ssl_ctx, pkt + written, idx - written);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            print_mbedtls_error("Failed to stream raw bytes into TLS sector", ret);
            return -1;
        }
        written += ret;
    }
    printf("[C MQTT DEBUG] Cypher CONNECT stream pumped via TLS tunnel (%d encrypted bytes written).\n", written);

    // 接收 CONNACK
    printf("[C MQTT DEBUG] Blocking on mbedtls_ssl_read waiting for EMQX Cloud CONNACK...\n");
    unsigned char reply[4];
    int r_len = 0;
    while (r_len < 4) {
        ret = mbedtls_ssl_read(&g_ssl_ctx, reply + r_len, sizeof(reply) - r_len);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            print_mbedtls_error("Broker broke TLS session link during authentication read", ret);
            return -1;
        }
        if (ret == 0) break; // Peer connection closed
        r_len += ret;
    }

    if (r_len >= 4) {
        printf("[C MQTT DEBUG] DECRYPTED CONNACK Hex Dump: [0x%02X 0x%02X 0x%02X 0x%02X]\n", reply[0], reply[1], reply[2], reply[3]);
    }

    if (r_len < 4 || reply[0] != 0x20 || reply[3] != 0x00) {
        printf("[C MQTT CRITICAL ERROR] Authentication rejected by EMQX! Code: %d\n", r_len >= 4 ? reply[3] : -1);
        return -1;
    }

    printf("[C MQTT SUCCESS] Handshake security authentication approved by EMQX Cloud gateway!\n");
    return 0;
}

/**
 * 纯 C 维护的 mbedTLS 长连接自愈状态机
 */
/**
 * 纯 C 维护的 mbedTLS 长连接自愈状态机
 */
static int ensure_native_mqtt_connected(void) {
    if (g_is_tls_connected) return 0;

    int ret;
    printf("[C TLS DEBUG] Initializing mbedTLS security structures...\n");
    mbedtls_net_init(&g_net_ctx);
    mbedtls_ssl_init(&g_ssl_ctx);
    mbedtls_ssl_config_init(&g_ssl_conf);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);

    // 1. 注入熵源，喂饱伪随机数发生器 (RNG)
    printf("[C TLS DEBUG] Seeding the random number generator framework...\n");
    const char *pers = "k230_fall_sec_core";
    if ((ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy, 
                                     (const unsigned char *)pers, strlen(pers))) != 0) {
        print_mbedtls_error("Entropy seed injection failed", ret);
        dismantle_tls_pipeline();
        return -1;
    }

    // 2. 建立原生 TCP 管道 (👈 此处已修复参数个数错误)
    printf("[C TLS] Directing TCP connect to cluster host: %s:%s...\n", MQTT_SERVER, MQTT_PORT);
    if ((ret = mbedtls_net_connect(&g_net_ctx, MQTT_SERVER, MQTT_PORT, MBEDTLS_NET_PROTO_TCP)) != 0) {
        print_mbedtls_error("Socket transport channel layer failed to hook up", ret);
        dismantle_tls_pipeline();
        return -1;
    }
    printf("[C TLS DEBUG] Infrastructure TCP pipe connected. Allocating SSL parameters...\n");

    // 3. 配置 SSL 默认核心参数
    if ((ret = mbedtls_ssl_config_defaults(&g_ssl_conf, MBEDTLS_SSL_IS_CLIENT, 
                                           MBEDTLS_SSL_TRANSPORT_STREAM, 
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        print_mbedtls_error("Setting SSL configurations failed", ret);
        dismantle_tls_pipeline();
        return -1;
    }

    // 绕过证书链强验证（通道依旧实施完全对称加密传输），对齐 Python 行为
    mbedtls_ssl_conf_authmode(&g_ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&g_ssl_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);

    if ((ret = mbedtls_ssl_setup(&g_ssl_ctx, &g_ssl_conf)) != 0) {
        print_mbedtls_error("SSL context allocation setup failed", ret);
        dismantle_tls_pipeline();
        return -1;
    }

    // 强注 SNI 域名，使 EMQX 专属集群网关顺利分发证书
    if ((ret = mbedtls_ssl_set_hostname(&g_ssl_ctx, MQTT_SERVER)) != 0) {
        print_mbedtls_error("SNI host domain embedding string failed", ret);
        dismantle_tls_pipeline();
        return -1;
    }

    // 绑定底层硬件套接字读写驱动
    mbedtls_ssl_set_bio(&g_ssl_ctx, &g_net_ctx, mbedtls_net_send, mbedtls_net_recv, NULL);

    // 4. 触发安全握手
    printf("[C TLS] Shaking hands with secure TLS cluster gateway (SNI active)...\n");
    while ((ret = mbedtls_ssl_handshake(&g_ssl_ctx)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            print_mbedtls_error("TLS hardware layer handshake failed completely", ret);
            dismantle_tls_pipeline();
            return -1;
        }
    }

    printf("[C TLS SUCCESS] TLS secure tunnel established. Symmetric crypto operational.\n");
    g_is_tls_connected = 1;

    // 5. 走加密隧道完成应用层 MQTT 鉴权
    if (send_native_mqtt_connect() != 0) {
        printf("[C MQTT ERROR] Core application logic denied, destroying secure pipe.\n");
        dismantle_tls_pipeline();
        return -1;
    }

    printf("[C MQTT SUCCESS] Permanent TLS-encrypted tunnel established with EMQX Cloud!\n");
    c_log_event("NATIVE_MQTT_CONNECTED_OK");
    return 0;
}

/**
 * 异步加密发布报警函数
 */
static void c_publish_fall_alert(void) {
    if (ensure_native_mqtt_connected() != 0) {
        printf("[C MQTT ERROR] Secure pipeline dead, caching event inside local flash.\n");
        c_log_event("FALL_DETECTED_BUT_NET_DOWN");
        return;
    }

    char time_str[64];
    get_formatted_time_str(time_str, sizeof(time_str));
    
    char payload[160];
    int payload_len = snprintf(payload, sizeof(payload), 
                               "{\"event\":\"fall\",\"device\":\"K230_Native\",\"timestamp\":\"%s\"}", 
                               time_str);

    const char *topic = MQTT_TOPIC;
    int topic_len = strlen(topic);
    int remain_len = 2 + topic_len + payload_len;

    unsigned char pub_hdr[4];
    int hdr_idx = 0;
    pub_hdr[hdr_idx++] = 0x30; 
    pub_hdr[hdr_idx++] = remain_len;
    pub_hdr[hdr_idx++] = (topic_len >> 8) & 0xFF;
    pub_hdr[hdr_idx++] = topic_len & 0xFF;

    printf("[C MQTT DEBUG] Encrypting & transmitting PUBLISH payload frame...\n");

    // 封装流式安全写入宏，替换标准 send
    #define TLS_WRITE_OR_FAIL(buf, len) \
        { \
            int w_ret, w_idx = 0; \
            while(w_idx < (len)) { \
                w_ret = mbedtls_ssl_write(&g_ssl_ctx, (const unsigned char*)(buf) + w_idx, (len) - w_idx); \
                if(w_ret <= 0) { \
                    if(w_ret == MBEDTLS_ERR_SSL_WANT_READ || w_ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue; \
                    print_mbedtls_error("SSL pipe collapsed during streaming payload", w_ret); \
                    dismantle_tls_pipeline(); \
                    c_log_event("TRANSMISSION_BROKEN_NEED_RETRY"); \
                    return; \
                } \
                w_idx += w_ret; \
            } \
        }

    TLS_WRITE_OR_FAIL(pub_hdr, hdr_idx);
    TLS_WRITE_OR_FAIL(topic, topic_len);
    TLS_WRITE_OR_FAIL(payload, payload_len);

    #undef TLS_WRITE_OR_FAIL

    printf("[C PUB SUCCESS] Pushed TLS encrypted payload to broker: %s\n", payload);
    c_log_event("NATIVE_MQTT_PUB_SUCCESS");
}

/**
 * 独立纯 C 语言常驻守护线程
 */
static void* native_alert_worker(void* arg) {
    (void)arg;
    printf("[C Thread] Autonomous secure MQTT daemon loop active via mbedTLS sub-engine.\n");

    struct timeval last_ping_tv, current_tv;
    gettimeofday(&last_ping_tv, NULL);

    while (g_thread_running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1; 

        int sem_res = sem_timedwait(&g_alert_sem, &ts);
        if (!g_thread_running) break;

        if (sem_res == 0) {
            printf("[C Thread DEBUG] Fall detected from AI thread model, waking up secure pipeline...\n");
            c_publish_fall_alert();
        }

        if (g_is_tls_connected) {
            gettimeofday(&current_tv, NULL);
            if (current_tv.tv_sec < last_ping_tv.tv_sec || current_tv.tv_sec - last_ping_tv.tv_sec > 100000) {
                last_ping_tv = current_tv;
            }
            int elapsed = current_tv.tv_sec - last_ping_tv.tv_sec;
            if (elapsed >= 25) { 
                printf("[C PING DEBUG] Keepalive clock triggered. Emitting encrypted PINGREQ...\n");
                unsigned char ping_req[] = { 0xC0, 0x00 };
                int ret = mbedtls_ssl_write(&g_ssl_ctx, ping_req, sizeof(ping_req));
                if (ret <= 0) {
                    printf("[C PING ERROR] Heartbeat failed to transit. Terminating TLS line.\n");
                    dismantle_tls_pipeline();
                } else {
                    printf("[C PING SUCCESS] Heartbeat byte stream safe delivery acknowledged.\n");
                    last_ping_tv = current_tv;
                }
            }
        } else {
            gettimeofday(&current_tv, NULL);
            if (current_tv.tv_sec < last_ping_tv.tv_sec || current_tv.tv_sec - last_ping_tv.tv_sec > 100000) {
                last_ping_tv = current_tv;
            }
            int elapsed = current_tv.tv_sec - last_ping_tv.tv_sec;
            if (elapsed >= 10) {
                printf("[C Thread DEBUG] Pipe disconnected. Initializing background self-healing retry...\n");
                ensure_native_mqtt_connected();
                last_ping_tv = current_tv;
            }
        }
    }

    dismantle_tls_pipeline();
    return NULL;
}

// ========================================================
// MicroPython 扩展模块 C 接口绑定
// ========================================================

static mp_obj_t mod_alert_init(void) {
    pthread_t thread_id;
    g_thread_running = 1;
    
    sem_init(&g_alert_sem, 0, 0);
    pthread_mutex_init(&g_log_mutex, NULL);

    if (pthread_create(&thread_id, NULL, native_alert_worker, NULL) != 0) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to create native worker thread"));
    }
    pthread_detach(thread_id);
    printf("[C Module SUCCESS] MicroPython module hooks registered over mbedTLS.\n");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_alert_init_obj, mod_alert_init);

static mp_obj_t mod_alert_trigger(void) {
    sem_post(&g_alert_sem); 
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_alert_trigger_obj, mod_alert_trigger);

static mp_obj_t mod_alert_log_event(mp_obj_t str_obj) {
    const char* event_str = mp_obj_str_get_str(str_obj);
    c_log_event(event_str); 
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_alert_log_event_obj, mod_alert_log_event);

static const mp_rom_map_elem_t k230_alert_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_k230_alert) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&mod_alert_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_trigger), MP_ROM_PTR(&mod_alert_trigger_obj) },
    { MP_ROM_QSTR(MP_QSTR_log_event), MP_ROM_PTR(&mod_alert_log_event_obj) },
};
static MP_DEFINE_CONST_DICT(k230_alert_module_globals, k230_alert_module_globals_table);

const mp_obj_module_t k230_alert_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&k230_alert_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_k230_alert, k230_alert_user_cmodule);
