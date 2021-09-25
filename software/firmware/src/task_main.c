#include "task_main.h"

#include "stm32l0xx_hal.h"
#include <cmsis_os.h>
#include <tusb.h>

#define LOG_TAG "task_main"
#include <elog.h>

#include "cdc_handler.h"
#include "settings.h"
#include "keypad.h"
#include "display.h"
#include "tsl2591.h"
#include "light.h"
#include "sensor.h"
#include "state_controller.h"

extern I2C_HandleTypeDef hi2c1;
extern SPI_HandleTypeDef hspi1;
extern TIM_HandleTypeDef htim2;

static void task_main_run(void *argument);
static void task_usbd_run(void *argument);

typedef struct {
    osThreadFunc_t task_func;
    osThreadAttr_t task_attrs;
    osThreadId_t task_handle;
} task_params_t;

static osSemaphoreId_t task_start_semaphore = NULL;
static const osSemaphoreAttr_t task_start_semaphore_attributes = {
    .name = "task_start_semaphore"
};

static task_params_t task_list[] = {
    {
        .task_func = task_main_run,
        .task_attrs = {
            .name = "main",
            .stack_size = 4096,
            .priority = osPriorityNormal
        }
    },
    {
        .task_func = task_usbd_run,
        .task_attrs = {
            .name = "usbd",
            .stack_size = 1536,
            .priority = osPriorityNormal2 // example uses max-1
        }
    },
    {
        .task_func = task_cdc_run,
        .task_attrs = {
            .name = "cdc",
            .stack_size = 1024,
            .priority = osPriorityNormal1 // example uses max-2
        }
    }
    //TODO task_keypad
    //TODO task_sensor
};


osStatus_t task_main_init()
{
    /* Create the semaphore used to synchronize task startup */
    task_start_semaphore = osSemaphoreNew(1, 0, &task_start_semaphore_attributes);
    if (!task_start_semaphore) {
        log_e("task_start_semaphore create error");
        return osErrorNoMemory;
    }

    /* Create the main task */
    task_list[0].task_handle = osThreadNew(task_list[0].task_func, NULL, &task_list[0].task_attrs);
    if (!task_list[0].task_handle) {
        log_e("main_task create error");
        return osErrorNoMemory;
    }
    return osOK;
}

void task_main_run(void *argument)
{
    const uint8_t task_count = sizeof(task_list) / sizeof(task_params_t);

    log_d("main_task start");

    /* Initialize the display */
    display_init(&hspi1);
    display_clear();

    /* Initialize the light sensor */
    sensor_init(&hi2c1);

    /* Initialize the light source */
    light_init(&htim2, TIM_CHANNEL_3, TIM_CHANNEL_4);

    /* Load system settings */
    settings_init();

    /* Initialize the state controller */
    state_controller_init();

    /* Loop through the remaining tasks and create them */
    for (uint8_t i = 1; i < task_count; i++) {
        /* Create the next task */
        task_list[i].task_handle = osThreadNew(task_list[i].task_func, task_start_semaphore, &task_list[i].task_attrs);
        if (!task_list[i].task_handle) {
            log_e("%s create error", task_list[i].task_attrs.name);
            continue;
        }

        /* Wait for the semaphore set once the task initializes */
        if (osSemaphoreAcquire(task_start_semaphore, portMAX_DELAY) != osOK) {
            log_e("Unable to acquire task_start_semaphore");
            return;
        }
    }

    /* Run the infinite main loop */
    log_i("Starting controller loop");
    state_controller_loop();
}

void task_usbd_run(void *argument)
{
    log_d("usbd_task start");

    /* Initialize the TinyUSB stack */
    if (!tusb_init()) {
        log_e("Unable to initialize tusb");
        return;
    }

    /* Enable USB interrupt */
    HAL_NVIC_EnableIRQ(USB_IRQn);

    /* Release the startup semaphore */
    if (osSemaphoreRelease(task_start_semaphore) != osOK) {
        log_e("Unable to release task_start_semaphore");
        return;
    }

    /* Run TinyUSB device task */
    while (1) {
        tud_task();
    }
}