/* *********************************************************
 * IoT dual light controller
 * Compatible with Web Thing API
 *
 *  Created on:		Sep 05, 2020
 * Last update:		Dec 16, 2020
 *      Author:		Krzysztof Zurek
 *		E-mail:		krzzurek@gmail.com
 *
 ************************************************************/
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "simple_web_thing_server.h"
#include "webthing_dual_light.h"

typedef enum {CH_A = 0, CH_B = 1, CH_AB = 2} channel_t;
#define APP_PERIOD 1000

//relays
#define GPIO_CH_A			(CONFIG_RELAY_A_GPIO)
#define GPIO_CH_B			(CONFIG_RELAY_B_GPIO)
#define GPIO_RELAY_MASK 	((1ULL << GPIO_CH_A) | (1ULL << GPIO_CH_B))
#define ESP_INTR_FLAG_DEFAULT 0

xSemaphoreHandle dual_light_mux;
xTaskHandle dual_light_task;

static bool init_data_sent = false;
static bool timer_is_running = false;
static channel_t current_channel, prev_current_channel;

//THINGS AND PROPERTIES
//------------------------------------------------------------
//--- Thing
thing_t *dual_light = NULL;

char dual_light_id_str[] = "Dual light";
char dual_light_attype_str[] = "Light";
char dual_light_disc[] = "Dual light relays";
at_type_t dual_light_type;

//------  property "on" - ON/OFF state
static bool device_is_on = false;
property_t *prop_on;
at_type_t on_prop_type;
int8_t set_on_off(char *new_value_str); //switch ON/OFF

char on_prop_id[] = "on";
char on_prop_disc[] = "on-off state";
char on_prop_attype_str[] = "OnOffProperty";
char on_prop_title[] = "ON/OFF";

//------  property "channel" - list of channels: A, B or AB
property_t *prop_channel;
at_type_t channel_prop_type;
enum_item_t enum_ch_A, enum_ch_B, enum_ch_AB;
int8_t set_channel(char *new_value_str);

char channel_prop_id[] = "channel";
char channel_prop_disc[] = "Channel";
char channel_prop_attype_str[] = "ChannelProperty";
char channel_prop_title[] = "Channel";
char channel_tab[3][4] = {"A", "B", "A+B"}; 

//------  property "daily_on" - daily on time
property_t *prop_daily_on_time;
at_type_t daily_on_prop_type;
action_input_prop_t *timer_duration;
static int daily_on_time_min = 0, daily_on_time_sec = 0;
static time_t on_time_last_update = 0;
static TimerHandle_t timer = NULL;
void update_on_time(bool);

char daily_on_prop_id[] = "daily_on";
char daily_on_prop_disc[] = "amount of time device is ON";
char daily_on_prop_attype_str[] = "LevelProperty";
char daily_on_prop_unit[] = "min";
char daily_on_prop_title[] = "ON minutes";

//------ action "timer"
action_t *timer_action;
int8_t timer_run(char *inputs);

char timer_id[] = "timer";
char timer_title[] = "Timer";
char timer_desc[] = "Turn ON device for specified period of time";
char timer_input_attype_str[] = "ToggleAction";
char timer_prop_dur_id[] = "duration";
char timer_duration_unit[] = "minutes";
double timer_duration_min = 1; //minutes
double timer_duration_max = 600;
at_type_t timer_input_attype;

//task function
void dual_light_fun(void *param); //thread function

//other functions
void read_nvs_data(void);
void write_nvs_data(int8_t data);


/* *****************************************************************
 *
 * turn the device ON or OFF
 *
 * *****************************************************************/
int8_t set_on_off(char *new_value_str){

	if (strcmp(new_value_str, "true") == 0){
		device_is_on = true;
		switch (current_channel){
			case CH_A:
				gpio_set_level(GPIO_CH_A, 1);
				break;
				
			case CH_B:
				gpio_set_level(GPIO_CH_B, 1);
				break;
				
			case CH_AB:
			default:
				gpio_set_level(GPIO_CH_A, 1);
				vTaskDelay(20 / portTICK_PERIOD_MS);
				gpio_set_level(GPIO_CH_B, 1);	
		}
	}
	else{
		device_is_on = false;

		gpio_set_level(GPIO_CH_A, 0);
		vTaskDelay(20 / portTICK_PERIOD_MS);
		gpio_set_level(GPIO_CH_B, 0);
	}

	return 1;
}


/******************************************************
 *
 * timer is finished, turn all channels OFF
 *
 * *****************************************************/
void timer_fun(TimerHandle_t xTimer){
	bool state_changed = false;
	
	//printf("timer fun\n");
	complete_action(0, "timer", ACT_COMPLETED);
	
	xSemaphoreTake(dual_light_mux, portMAX_DELAY);

	if (device_is_on == true){
		//switch OFF both channels
		device_is_on = false;
		gpio_set_level(GPIO_CH_A, 0);
		vTaskDelay(20 / portTICK_PERIOD_MS);
		gpio_set_level(GPIO_CH_B, 0);
		state_changed = true;
	}
	xSemaphoreGive(dual_light_mux);
	
	xTimerDelete(xTimer, 100); //delete timer
	timer_is_running = false;
	
	if (state_changed == true){
		inform_all_subscribers_prop(prop_on);
	}
}


/**********************************************************
 *
 * timer action
 * inputs:
 * 		- minutes of turn ON in json, e.g.: "duration":10
 *
 * *******************************************************/
int8_t timer_run(char *inputs){
	int duration = 0, len;
	char *p1, buff[6];
	bool switched_on = false;

	if (timer_is_running == true){
		goto inputs_error;
	}
	
	//get duration value
	p1 = strstr(inputs, "duration");
	if (p1 == NULL){
		goto inputs_error;
	}
	p1 = strchr(p1, ':');
	if (p1 == NULL){
		goto inputs_error;
	}
	len = strlen(inputs) - (p1 + 1 - inputs);
	if (len > 5){
		goto inputs_error;
	}
	memset(buff, 0, 6);
	memcpy(buff, p1 + 1, len);
	duration = atoi(buff);
	if ((duration > 600) || (duration == 0)){
		goto inputs_error;
	}
	
	xSemaphoreTake(dual_light_mux, portMAX_DELAY);
	if (device_is_on == false){
		device_is_on = true; //if device is OFF switch it ON now
		switched_on = true;
	
		//check current channel
		switch (current_channel){
			case CH_A:
				gpio_set_level(GPIO_CH_A, 1);
				break;
		
			case CH_B:
				gpio_set_level(GPIO_CH_B, 1);
				break;
			
			case CH_AB:
			default:
				gpio_set_level(GPIO_CH_A, 1);
				vTaskDelay(20 / portTICK_PERIOD_MS);
				gpio_set_level(GPIO_CH_B, 1);
		}
	}
	//start timer
	timer = xTimerCreate("timer",
						pdMS_TO_TICKS(duration * 60 * 1000),
						pdFALSE,
						pdFALSE,
						timer_fun);

	xSemaphoreGive(dual_light_mux);
	
	if (xTimerStart(timer, 5) == pdFAIL){
		printf("timer failed\n");
	}
	else{
		timer_is_running = true;
		if (switched_on == true){
			inform_all_subscribers_prop(prop_on);
		}
	}

	return 0;

	inputs_error:
		printf("timer ERROR\n");
	return -1;
}


/*******************************************************************
*
* set channel, called after http PUT method
* output:
* 	0 - value is ok, but not changed (the same as previous one)
* 	1 - value is changed, subscribers will be informed
* < 0 - error
*
*******************************************************************/
int8_t set_channel(char *new_value_str){
	int8_t channel_is_changed = -1;
	char *buff = NULL;
	
	//in websocket quotation mark is not removed
	//(in http should be the same but is not)
	buff = malloc(strlen(new_value_str));
	if (new_value_str[0] == '"'){
		memset(buff, 0, strlen(new_value_str));
		char *ptr = strchr(new_value_str + 1, '"');
		int len = ptr - new_value_str - 1;
		memcpy(buff, new_value_str + 1, len);
	}
	else{
		strcpy(buff, new_value_str);
	}
	
	//set channel
	if (prop_channel -> enum_list != NULL){
		int i = 0;
		enum_item_t *enum_item = prop_channel -> enum_list;
		while (enum_item != NULL){
			if (strcmp(buff, enum_item -> value.str_addr) == 0){
				prop_channel -> value = enum_item -> value.str_addr;
				if (i != current_channel){
					prev_current_channel = current_channel;
					current_channel = i;
					channel_is_changed = 1;
				}
				else{
					channel_is_changed = 0;
				}
				break;
			}
			else{
				enum_item = enum_item -> next;
				i++;
			}
		}
	}

	//if channel changed when device is ON then switch OFF previous channel
	//and switch ON new channel
	if ((channel_is_changed == 1) && (device_is_on == true)){
		switch (prev_current_channel){
			case CH_A:
				if (current_channel == CH_B){
					gpio_set_level(GPIO_CH_A, 0);
					vTaskDelay(20 / portTICK_PERIOD_MS);
					gpio_set_level(GPIO_CH_B, 1);
				}
				else{
					gpio_set_level(GPIO_CH_B, 1);
				}
				break;
				
			case CH_B:
				if (current_channel == CH_A){
					gpio_set_level(GPIO_CH_B, 0);
					vTaskDelay(20 / portTICK_PERIOD_MS);
					gpio_set_level(GPIO_CH_A, 1);
				}
				else{
					gpio_set_level(GPIO_CH_A, 1);
				}
				break;
				
			case CH_AB:
			default:
				if (current_channel == CH_A){
					gpio_set_level(GPIO_CH_B, 0);
				}
				else{
					gpio_set_level(GPIO_CH_A, 0);
				}									
		}
	}
	free(buff);
	
	if (channel_is_changed == 1){
		write_nvs_data((int8_t)current_channel);
	}
	
	return channel_is_changed;
}


/*********************************************************************
 *
 * main task
 *
 * ******************************************************************/
void dual_light_fun(void *param){
	
	TickType_t last_wake_time = xTaskGetTickCount();
	for (;;){
		last_wake_time = xTaskGetTickCount();
		
		update_on_time(false);
		
		if (init_data_sent == false){
			int8_t s1 = inform_all_subscribers_prop(prop_channel);
			int8_t s2 = inform_all_subscribers_prop(prop_on);
			int8_t s3 = inform_all_subscribers_prop(prop_daily_on_time);
			if ((s1 == 0) && (s2 == 0) && (s3 == 0)){
				init_data_sent = true;
			}
		}
	
		vTaskDelayUntil(&last_wake_time, APP_PERIOD / portTICK_PERIOD_MS);
	}
}


/***************************************************************
*
* daily ON time update and inform subscribers if necessary
*
****************************************************************/
void update_on_time(bool reset){
	struct tm timeinfo;
	int delta_t = 0, prev_minutes = 0, new_minutes = 0;
	time_t current_time, prev_time;
	bool send_data = false;

	prev_time = on_time_last_update;
	time(&current_time);
	localtime_r(&current_time, &timeinfo);
	if (timeinfo.tm_year > (2018 - 1900)) {
		//time is correct
		xSemaphoreTake(dual_light_mux, portMAX_DELAY);
		if (device_is_on == true){
			prev_minutes = daily_on_time_min;
			new_minutes = prev_minutes;
			delta_t = current_time - prev_time;
			if (delta_t > 0){
				daily_on_time_sec += delta_t;
				daily_on_time_min = daily_on_time_sec / 60;
				new_minutes = daily_on_time_min;
			}	
		}
		on_time_last_update = current_time;
		xSemaphoreGive(dual_light_mux);
		
		if (new_minutes != prev_minutes){
			send_data = true;
		}
		
		if (reset == true){
			xSemaphoreTake(dual_light_mux, portMAX_DELAY);
			daily_on_time_sec = 0;
			daily_on_time_min = 0;
			xSemaphoreGive(dual_light_mux);
			send_data = true;
		}
		
		if (send_data == true){
			inform_all_subscribers_prop(prop_daily_on_time);
		}
    }
}


/*************************************************************
*
* at the beginning of the day reset minuts and seconds counters
* and inform subscribers if necessary
*
**************************************************************/
void daily_on_time_reset(void){
	update_on_time(true);
}


/*******************************************************************
 *
 * initialize GPIOs for channel A and B, both switch OFF
 *
 * ******************************************************************/
void init_gpio(void){
	gpio_config_t io_conf;

	//disable interrupt
	io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
	//bit mask of the pins
	io_conf.pin_bit_mask = GPIO_RELAY_MASK;
	//set as output mode
	io_conf.mode = GPIO_MODE_OUTPUT;
	//disable pull-up mode
	io_conf.pull_up_en = 0;
	io_conf.pull_down_en = 0;
	gpio_config(&io_conf);
	
	gpio_set_level(GPIO_CH_A, 0);
	gpio_set_level(GPIO_CH_B, 0); //led is off
}


/*****************************************************************
 *
 * Initialization of dual light thing and all it's properties
 *
 * ****************************************************************/
thing_t *init_dual_light(void){

	read_nvs_data();
	prev_current_channel = current_channel;
	
	init_gpio();
	
	//start thing
	dual_light_mux = xSemaphoreCreateMutex();
	//create thing 1, thermostat ---------------------------------
	dual_light = thing_init();

	dual_light -> id = dual_light_id_str;
	dual_light -> at_context = things_context;
	dual_light -> model_len = 2300;
	//set @type
	dual_light_type.at_type = dual_light_attype_str;
	dual_light_type.next = NULL;
	set_thing_type(dual_light, &dual_light_type);
	dual_light -> description = dual_light_disc;
	
	//property: ON/OFF
	prop_on = property_init(NULL, NULL);
	prop_on -> id = "on";
	prop_on -> description = "ON/OFF";
	on_prop_type.at_type = "OnOffProperty";
	on_prop_type.next = NULL;
	prop_on -> at_type = &on_prop_type;
	prop_on -> type = VAL_BOOLEAN;
	prop_on -> value = &device_is_on;
	prop_on -> title = "ON/OFF";
	prop_on -> read_only = false;
	prop_on -> set = set_on_off;
	prop_on -> mux = dual_light_mux;
	add_property(dual_light, prop_on); //add property to thing
	
	//create "channel" property ------------------------------------
	//pop-up list to choose channel
	prop_channel = property_init(NULL, NULL);
	prop_channel -> id = channel_prop_id;
	prop_channel -> description = channel_prop_disc;
	channel_prop_type.at_type = channel_prop_attype_str;
	channel_prop_type.next = NULL;
	prop_channel -> at_type = &channel_prop_type;
	prop_channel -> type = VAL_STRING;
	prop_channel -> value = channel_tab[current_channel];
	prop_channel -> title = channel_prop_title;
	prop_channel -> read_only = false;
	prop_channel -> enum_prop = true;
	prop_channel -> enum_list = &enum_ch_A;
	enum_ch_A.value.str_addr = channel_tab[0];
	enum_ch_A.next = &enum_ch_B;
	enum_ch_B.value.str_addr = channel_tab[1];
	enum_ch_B.next = &enum_ch_AB;
	enum_ch_AB.value.str_addr = channel_tab[2];
	enum_ch_AB.next = NULL;
	prop_channel -> set = &set_channel;
	prop_channel -> mux = dual_light_mux;

	add_property(dual_light, prop_channel); //add property to thing	
	
	//create "daily on time" property -------------------------------------------
	prop_daily_on_time = property_init(NULL, NULL);
	prop_daily_on_time -> id = daily_on_prop_id;
	prop_daily_on_time -> description = daily_on_prop_disc;
	daily_on_prop_type.at_type = daily_on_prop_attype_str;
	daily_on_prop_type.next = NULL;
	prop_daily_on_time -> at_type = &daily_on_prop_type;
	prop_daily_on_time -> type = VAL_INTEGER;
	prop_daily_on_time -> value = &daily_on_time_min;
	prop_daily_on_time -> unit = daily_on_prop_unit;
	prop_daily_on_time -> max_value.int_val = 1440;
	prop_daily_on_time -> min_value.int_val = 0;
	prop_daily_on_time -> title = daily_on_prop_title;
	prop_daily_on_time -> read_only = true;
	prop_daily_on_time -> enum_prop = false;
	prop_daily_on_time -> set = NULL;
	prop_daily_on_time -> mux = dual_light_mux;
	
	add_property(dual_light, prop_daily_on_time); //add property to thing
	
	//create action "timer", turn on lights (device) for specified minutes
	timer_action = action_init();
	timer_action -> id = timer_id;
	timer_action -> title = timer_title;
	timer_action -> description = timer_desc;
	timer_action -> run = timer_run;
	timer_input_attype.at_type = timer_input_attype_str;
	timer_input_attype.next = NULL;
	timer_action -> input_at_type = &timer_input_attype;
	timer_duration = action_input_prop_init(timer_prop_dur_id,
			VAL_INTEGER, true, &timer_duration_min, &timer_duration_max,
			timer_duration_unit);
	add_action_input_prop(timer_action, timer_duration);
	add_action(dual_light, timer_action);

	//start thread	
	xTaskCreate(&dual_light_fun, "dual_light", configMINIMAL_STACK_SIZE * 4, NULL, 5, &dual_light_task);

	return dual_light;
}


/****************************************************************
 *
 * read dual light data written in NVS memory:
 *  - current channel
 *
 * **************************************************************/
void read_nvs_data(void){
	esp_err_t err;
	nvs_handle storage_handle = 0;

	//default values
	current_channel = CH_AB;

	// Open
	printf("Reading NVS data... ");

	err = nvs_open("storage", NVS_READONLY, &storage_handle);
	if (err != ESP_OK) {
		printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
	}
	else {
		// Read data
		if (nvs_get_i8(storage_handle, "curr_channel", (int8_t *)&current_channel) != ESP_OK){
			printf("current channel not found in NVS\n");
		}
		// Close
		nvs_close(storage_handle);
	}
}


/****************************************************************
 *
 * write NVS data into flash memory
 * input:
 *  current channel
 *
 * **************************************************************/
void write_nvs_data(int8_t data){
	esp_err_t err;
	nvs_handle storage_handle = 0;

	//open NVS falsh memory
	err = nvs_open("storage", NVS_READWRITE, &storage_handle);
	if (err != ESP_OK) {
		printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
	}
	else {
		nvs_set_i8(storage_handle, "curr_channel", data);
		
		err = nvs_commit(storage_handle);
		// Close
		nvs_close(storage_handle);
	}
}
