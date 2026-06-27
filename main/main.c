/*
 * Multi-Sensor Data Acquisition & Monitoring System
 * ESP-IDF 5.2.7 | FreeRTOS | UART telemetry (no WiFi/MQTT needed)
 *
 * Demonstrates: QueueSet, EventGroup sync barrier, Mutex (priority
 * inheritance), Task Notifications (fault path), Software Timer
 * (heartbeat), MessageBuffer (single-owner UART TX), Watchdog.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/message_buffer.h"
#include "esp_task_wdt.h"
#include "esp_random.h"
#include "esp_log.h"

/*
 * NOTE ON UART: ESP-IDF already uses UART0 as the console UART for
 * flashing and the serial monitor. We deliberately do NOT call
 * uart_driver_install() again on it (that would conflict with the
 * console driver). Instead we write our "telemetry" bytes straight to
 * stdout, which IS UART0 under the hood. You see everything in the
 * VS Code ESP-IDF "Monitor" pane with zero extra wiring.
 *
 * If you later get a USB-TTL adapter, swap UartTxTask's fwrite() call
 * for uart_write_bytes() on UART_NUM_1/2 with dedicated GPIO pins —
 * the rest of the architecture (message buffer, aggregator, etc.)
 * doesn't change at all. That's a good "what would you change for
 * production" answer in interviews.
 */

#define SENSOR_QUEUE_LEN   5
#define MSG_BUF_SIZE       1024

typedef struct {
    const char *name;
    float value;
} sensor_reading_t;

//======================= Globals =======================
static QueueHandle_t       q_temp, q_humid, q_vibro;
static QueueSetHandle_t    qset_sensors;
static EventGroupHandle_t  eg_ready;
static SemaphoreHandle_t   mtx_console;
static MessageBufferHandle_t msgbuf_uart;
static TimerHandle_t       heartbeat_timer;
static TaskHandle_t        fault_task_handle = NULL;

#define BIT_TEMP_READY  (1 << 0)
#define BIT_HUMID_READY (1 << 1)
#define BIT_VIBRO_READY (1 << 2)
#define ALL_READY_BITS  (BIT_TEMP_READY | BIT_HUMID_READY | BIT_VIBRO_READY)

//======================= Helper: thread-safe debug print =======================
static void safe_print(const char *fmt, ...)
{
    xSemaphoreTake(mtx_console, portMAX_DELAY);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    xSemaphoreGive(mtx_console);
}

//======================= Sensor Tasks (Producers) =======================
// Each one: announces readiness via event group, then loops pushing
// readings into its own queue. If out of range, notifies FaultTask
// directly instead of going through a queue (cheap, urgent signal).

static void TempTask(void *pv)
{
    xEventGroupSetBits(eg_ready, BIT_TEMP_READY);
    while (1) {
        sensor_reading_t r = { .name = "TEMP", .value = 20.0f + (esp_random() % 200) / 10.0f };
        xQueueSendToBack(q_temp, &r, 0);
        if (r.value > 35.0f) {
            xTaskNotify(fault_task_handle, BIT_TEMP_READY, eSetBits);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void HumidTask(void *pv)
{
    xEventGroupSetBits(eg_ready, BIT_HUMID_READY);
    while (1) {
        sensor_reading_t r = { .name = "HUMID", .value = 30.0f + (esp_random() % 500) / 10.0f };
        xQueueSendToBack(q_humid, &r, 0);
        if (r.value > 70.0f) {
            xTaskNotify(fault_task_handle, BIT_HUMID_READY, eSetBits);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

static void VibroTask(void *pv)
{
    xEventGroupSetBits(eg_ready, BIT_VIBRO_READY);
    while (1) {
        sensor_reading_t r = { .name = "VIBRO", .value = (esp_random() % 100) / 10.0f };
        xQueueSendToBack(q_vibro, &r, 0);
        if (r.value > 8.0f) {
            xTaskNotify(fault_task_handle, BIT_VIBRO_READY, eSetBits);
        }
        vTaskDelay(pdMS_TO_TICKS(800));
    }
}

//======================= Fault Task (Notification consumer) =======================
static void FaultTask(void *pv)
{
    uint32_t notified;
    while (1) {
        xTaskNotifyWait(0x00, ULONG_MAX, &notified, portMAX_DELAY);

        if (notified & BIT_TEMP_READY)
            safe_print("[FAULT] Temperature out of range!\n");
        if (notified & BIT_HUMID_READY)
            safe_print("[FAULT] Humidity out of range!\n");
        if (notified & BIT_VIBRO_READY)
            safe_print("[FAULT] Vibration threshold exceeded!\n");
    }
}

//======================= Heartbeat Timer Callback =======================
static void heartbeat_cb(TimerHandle_t xTimer)
{
    char hb[] = "HEARTBEAT,ALIVE\n";
    // Non-blocking push into the UART message buffer; timers run on the
    // daemon task so we must never block here.
    xMessageBufferSend(msgbuf_uart, hb, strlen(hb), 0);
}

//======================= Aggregator Task (QueueSet consumer) =======================
// Waits on the gate (all sensors ready), then drains whichever queue has
// data via the queue set, formats a line, and forwards it to the single
// UART-owning message buffer.
static void AggregatorTask(void *pv)
{
    // Register with watchdog: if this task ever stalls, the chip resets.
    esp_task_wdt_add(NULL);

    safe_print("[SYS] Waiting for all sensors to come online...\n");
    xEventGroupWaitBits(eg_ready, ALL_READY_BITS, pdFALSE, pdTRUE, portMAX_DELAY);
    safe_print("[SYS] All sensors ready. Starting aggregation.\n");

    char line[80];
    sensor_reading_t r;

    while (1) {
        QueueSetMemberHandle_t active =
            xQueueSelectFromSet(qset_sensors, pdMS_TO_TICKS(2000));

        if (active != NULL) {
            xQueueReceive(active, &r, 0);
            int len = snprintf(line, sizeof(line), "%s = %.2f\n", r.name, r.value);
            xMessageBufferSend(msgbuf_uart, line, len, pdMS_TO_TICKS(100));
        }

        // Feed the watchdog every loop iteration (data or timeout, doesn't matter)
        esp_task_wdt_reset();
    }
}

//======================= UART TX Task (sole owner of the peripheral) =======================
static void UartTxTask(void *pv)
{
    char buf[128];
    while (1) {
        size_t n = xMessageBufferReceive(msgbuf_uart, buf, sizeof(buf), portMAX_DELAY);
        if (n > 0) {
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
        }
    }
}

//======================= app_main =======================
void app_main(void)
{
    // --- Sync primitives ---
    eg_ready     = xEventGroupCreate();
    mtx_console  = xSemaphoreCreateMutex();
    msgbuf_uart  = xMessageBufferCreate(MSG_BUF_SIZE);

    q_temp  = xQueueCreate(SENSOR_QUEUE_LEN, sizeof(sensor_reading_t));
    q_humid = xQueueCreate(SENSOR_QUEUE_LEN, sizeof(sensor_reading_t));
    q_vibro = xQueueCreate(SENSOR_QUEUE_LEN, sizeof(sensor_reading_t));

    qset_sensors = xQueueCreateSet(SENSOR_QUEUE_LEN * 3);
    xQueueAddToSet(q_temp,  qset_sensors);
    xQueueAddToSet(q_humid, qset_sensors);
    xQueueAddToSet(q_vibro, qset_sensors);

    // --- Tasks ---
    xTaskCreate(FaultTask,      "FaultTask",  1024 * 4, NULL, 3, &fault_task_handle);
    xTaskCreate(AggregatorTask, "Aggregator", 1024 * 4, NULL, 2, NULL);
    xTaskCreate(UartTxTask,     "UartTx",     1024 * 4, NULL, 2, NULL);
    xTaskCreate(TempTask,       "TempTask",   1024 * 3, NULL, 1, NULL);
    xTaskCreate(HumidTask,      "HumidTask",  1024 * 3, NULL, 1, NULL);
    xTaskCreate(VibroTask,      "VibroTask",  1024 * 3, NULL, 1, NULL);

    // --- Heartbeat timer (every 5s) ---
    heartbeat_timer = xTimerCreate("Heartbeat", pdMS_TO_TICKS(5000), pdTRUE, NULL, heartbeat_cb);
    xTimerStart(heartbeat_timer, 0);
}
