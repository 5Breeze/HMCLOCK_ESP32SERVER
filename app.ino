#include <Arduino.h>
#include <NimBLEDevice.h>
#include <map>
#include <string>
#include <WiFi.h> 
#include <time.h>
#include <WebServer.h>
#include <Preferences.h>

// 配网相关定义
#define AP_SSID "DLG-Clock-Config"
#define AP_PASS "config123"
#define CONFIG_PORT 80
WebServer server(CONFIG_PORT);
Preferences preferences;

// WiFi配置存储键名
#define WIFI_SSID_KEY "wifi_ssid"
#define WIFI_PASS_KEY "wifi_pass"

// 全局WiFi配置变量
String ssid;
String password;

// NTP服务器（可选，国内推荐阿里云/腾讯云服务器）
const char* ntpServer = "ntp.aliyun.com";
const long gmtOffset_sec = 8 * 3600;  // 时区：东8区（+8小时）
const int daylightOffset_sec = 0;     // 夏令时：无


// 设备和服务配置
#define DEVICE_PREFIX "DLG-CLOCK"       // 目标设备名称前缀
#define SERVICE_UUID  "0000ff00-0000-1000-8000-00805f9b34fb"  // 目标服务UUID
#define CHAR_UUID     "0000ff01-0000-1000-8000-00805f9b34fb"  // 目标特征UUID

// 扫描和连接参数
static const uint32_t scanTimeMs = 5000;       // 单次扫描时长(ms)
static const uint32_t SKIP_DURATION = 3600000;  // 跳过已连接设备的时间(ms，60分钟)

// 全局状态变量
static const NimBLEAdvertisedDevice* advDevice = nullptr;  // 存储发现的目标设备
static bool doConnect = false;                             // 连接触发标志
static std::map<std::string, unsigned long> recentConnections;  // 最近连接记录

// 配网页面HTML
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
    <meta charset="UTF-8">  <!-- 新增：指定字符编码为UTF-8 -->
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>DLG时钟配网</title>
    <style>
        body { font-family: Arial; text-align: center; margin: 0; padding: 20px; }
        .container { max-width: 300px; margin: 0 auto; text-align: left; }
        input { width: 100%; padding: 8px; margin: 10px 0; box-sizing: border-box; }
        button { width: 100%; padding: 10px; background: #4CAF50; color: white; border: none; border-radius: 4px; cursor: pointer; }
        button:hover { background: #45a049; }
    </style>
</head>
<body>
    <h1>DLG时钟配网</h1>
    <div class="container">
        <form action="/save" method="POST">
            <label>WiFi名称 (SSID)</label>
            <input type="text" name="ssid" required>
            
            <label>WiFi密码</label>
            <input type="password" name="pass">
            
            <button type="submit">保存配置</button>
        </form>
    </div>
</body>
</html>
)rawliteral";

// 从存储中加载WiFi配置
void loadWiFiConfig() {
    preferences.begin("wifi_config", true);
    ssid = preferences.getString(WIFI_SSID_KEY, "");
    password = preferences.getString(WIFI_PASS_KEY, "");
    preferences.end();
}

// 保存WiFi配置到存储
void saveWiFiConfig(String newSsid, String newPass) {
    preferences.begin("wifi_config", false);
    preferences.putString(WIFI_SSID_KEY, newSsid);
    preferences.putString(WIFI_PASS_KEY, newPass);
    preferences.end();
    
    ssid = newSsid;
    password = newPass;
}

// 启动配网AP模式
void startConfigAP() {
    Serial.println("启动配网模式...");
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("AP IP地址: ");
    Serial.println(WiFi.softAPIP());
    
    // 路由设置
    server.on("/", []() {
        server.send(200, "text/html", index_html);
    });
    
    server.on("/save", []() {
        if (server.hasArg("ssid")) {
            String newSsid = server.arg("ssid");
            String newPass = server.arg("pass");
            
            saveWiFiConfig(newSsid, newPass);
            server.send(200, "text/plain", "Rebooting......");
            
            // 重启设备应用新配置
            delay(2000);
            ESP.restart();
        } else {
            server.send(400, "text/plain", "参数错误");
        }
    });
    
    server.begin();
    Serial.println("配网服务已启动");
}

// 连接WiFi函数
bool connectWiFi() {
    if (ssid.isEmpty()) {
        Serial.println("未配置WiFi信息，进入配网模式");
        return false;
    }
    
    Serial.printf("连接WiFi: %s ...", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        timeout++;
        if (timeout > 20) {  // 10秒超时
            Serial.println("\nWiFi连接超时");
            return false;
        }
    }
    
    Serial.println("\n✅ WiFi连接成功，IP地址: " + WiFi.localIP().toString());
    return true;
}

// NTP时间同步函数
void syncNtpTime() {
    // 配置时间：NTP服务器、时区
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    time_t now_t = time(nullptr);
    int retry = 0;
    // 等待NTP同步（最多重试10次）
    while (now_t < 1609459200 && retry < 10) {
        delay(1000);
        Serial.printf("等待NTP同步...（%d/10）\n", retry+1);
        now_t = time(nullptr);
        retry++;
    }
    
    if (now_t >= 1609459200) {
        struct tm timeinfo;
        localtime_r(&now_t, &timeinfo);
        Serial.printf("✅ NTP同步成功，当前时间: %d-%02d-%02d %02d:%02d:%02d\n",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println("❌ NTP同步失败，请检查网络");
    }
}

// 客户端回调：处理连接/断开事件
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        Serial.printf("✅ 已连接设备: %s\n", pClient->getPeerAddress().toString().c_str());
    }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        Serial.printf("❌ 设备 %s 断开连接，原因: %d，重启扫描...\n", 
                     pClient->getPeerAddress().toString().c_str(), reason);
        // 断开后自动重启扫描
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
    }
} clientCallbacks;

// 扫描回调：发现设备时触发
class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        std::string devName = advertisedDevice->getName();
        std::string devAddr = advertisedDevice->getAddress().toString();
        
        // 打印发现的设备信息
        Serial.printf("🔍 发现设备: 名称=%s, 地址=%s, RSSI=%ddBm\n",
                     devName.c_str(), devAddr.c_str(), advertisedDevice->getRSSI());

        // 过滤目标设备（名称前缀匹配+10分钟内未连接过）
        if (devName.find(DEVICE_PREFIX) == 0) {
            unsigned long now = millis();
            // 检查是否需要跳过（10分钟内已连接）
            if (recentConnections.count(devAddr) && (now - recentConnections[devAddr] < SKIP_DURATION)) {
                Serial.printf("⏭️ 设备 %s 60分钟内已连接过，跳过\n", devAddr.c_str());
                return;
            }

            // 找到目标设备，准备连接
            Serial.printf("🎯 找到目标设备: %s，准备连接...\n", devName.c_str());
            NimBLEDevice::getScan()->stop();  // 停止扫描
            advDevice = advertisedDevice;     // 保存设备信息
            doConnect = true;                 // 触发连接
        }
    }

    // 扫描结束后重启（未找到目标设备时）
    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        Serial.printf("📋 扫描结束，原因: %d，共发现%d个设备，重启扫描...\n", 
                     reason, results.getCount());
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
    }
} scanCallbacks;


struct c_calender { int year, month, day, is_leap_month; };

/* 1900-2051年农历数据 */
static const unsigned int calendar_data[] = {
0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, 
0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, 
0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, 
0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950, 
0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, 
0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, 
0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, 
0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6, 
0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, 
0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x055c0, 0x0ab60, 0x096d5, 0x092e0, 
0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, 
0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930, 
0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, 
0x05aa0, 0x076a3, 0x096d0, 0x04bd7, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, 
0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, 
0x14b63};

/* 农历相关函数 */
#define NIAN_BIT(nian, bit)(calendar_data[nian - 1900] & bit)
#define WHICH_RUN_YUE(nian)(NIAN_BIT(nian, 0xF))

static int days_of_run_rue(int nian)
{
	if(WHICH_RUN_YUE(nian))
		return (NIAN_BIT(nian, 0x10000) > 0)?30:29;
	else
		return 0;
}

static int days_of_nian(int nian)
{
	int i;
	int sum = 348;

	for (i=0x8000; i>8; i>>=1)
		if (NIAN_BIT(nian, i) > 0) sum++;
	return sum + days_of_run_rue(nian);
}

static int is_bissextile(int year)
{
	if ((year % 4==0 && year % 100!=0) || year % 400==0)
		 return 1;
	else
		 return 0;
}

static int sum_to_premonth(int year, int month)
{
	int i, sum = (month - 1)?(month - 1) * 30:0;

	for (i=1; i<=month - 1; i++)
		switch(i) {
			case 1: case 3: case 5: case 7: 
			case 8: case 10: case 12:
				sum++;
				break;
			case 2 :
				sum -= 2;
				if (is_bissextile(year)) sum++;
				break;
		}
	return sum;
}

static int sub_two_date(int year1, int month1, int day1,
				int year2, int month2, int day2)
{
	int i;

	int sum = sum_to_premonth(year1, month1) + 
			365 - sum_to_premonth(year2, month2);
	if (is_bissextile(year2)) sum--;
	sum += day1 - day2;

	for (i=year2+1; i<year1; i++)
		if (is_bissextile(i)) sum++;
	if ((year1 - year2) > 0)
		sum += (year1 - year2 - 1) * 365;
	else if (year1 == year2)
		sum -= 365;
	else
		return -1;

	return sum;
}

int chinese_calender(int year, int month, int day, int hour,
							  struct c_calender *d)
{
	static int percalc_val[] = {0, 18279, 36529};

	unsigned int i, x, run_yue, is_run_yue = 0;

	int all_days = sub_two_date(year, month, day, 1900, 1, 31);

	if (year < 1901 || year > 2050)
		 return 1;

	for (i=0; i<5; i++)
		if ((year > (i * 50+1900)) && (year <= ((i + 1) * 50+1900)))
			 break;

	all_days -= percalc_val[i];

	for (i=i*50+1900; (all_days>0)&&(i<2050); i++) {
		x = days_of_nian(i);
		all_days -= x;
	}

	if (all_days < 0) {
		 all_days += x;
		 i--;
	}

	d->year = i;

	run_yue = WHICH_RUN_YUE(i);

	for (i=1; i<13 && all_days>0; i++) {
		if ((run_yue > 0) && (i == run_yue + 1) && (is_run_yue == 0)) {
			--i;
			is_run_yue = 1;
			x = days_of_run_rue(d->year);
		}
		else
			x = NIAN_BIT(d->year, 0x10000 >> i)?30:29;
		all_days -= x;
		if (is_run_yue == 1 && i == (run_yue + 1)) is_run_yue = 0;
	}
	if (all_days < 0) {
		 all_days += x;
		 i--;
	}
	
	if ((all_days == 0) && (run_yue > 0) && (i == run_yue + 1)) {
		if (is_run_yue == 1)
			is_run_yue = 0;
		else {
			is_run_yue = 1;
			--i;
		}
	}

	d->month = i;
	d->day = all_days + 1;
	d->is_leap_month = is_run_yue;
	return 0;
}

// 时间同步核心函数：向设备写入当前时间
bool syncTimeToDevice(NimBLEClient* pClient) {
    // 获取目标服务
    NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (!pService) {
        Serial.printf("❌ 未找到目标服务: %s\n", SERVICE_UUID);
        return false;
    }

    // 获取目标特征
    NimBLERemoteCharacteristic* pChar = pService->getCharacteristic(CHAR_UUID);
    if (!pChar) {
        Serial.printf("❌ 未找到目标特征: %s\n", CHAR_UUID);
        return false;
    }

    // 检查特征是否可写
    if (!pChar->canWrite()) {
        Serial.println("❌ 目标特征不可写");
        return false;
    }

    // 获取当前时间（需确保设备时间有效，可通过NTP/RTC同步）
    time_t now_t = time(nullptr);
    if (now_t < 1609459200) {  // 检查是否为2021年之后的有效时间
        Serial.println("❌ 设备时间无效，请先同步系统时间");
        return false;
    }

    struct tm* now = localtime(&now_t);
    if (!now) {
        Serial.println("❌ 解析时间失败");
        return false;
    }

    int lyear, lmonth, lday;
    lyear=(now->tm_year + 1900);
    lmonth=(now->tm_mon + 1);
    lday=now->tm_mday;
    struct c_calender lunar;
    chinese_calender(lyear, lmonth, lday, 0, &lunar);
    int l_month = lunar.is_leap_month ? 128 + lunar.month : lunar.month;
    Serial.println(lunar.year);
    Serial.println(lunar.month);
    Serial.println(lunar.day);
    Serial.println(l_month);
    
    // 构造对时命令（修正月份：tm_mon是0-11，需+1）
    uint8_t timeBuf[12] = {
        0x91,  // 指令头
        (uint8_t)(now->tm_year + 1900) % 256,  // 年份低8位
        (uint8_t)((now->tm_year + 1900) / 256), // 年份高8位
        (uint8_t)(now->tm_mon + 1),            // 月份（1-12）
        (uint8_t)now->tm_mday,                 // 日
        (uint8_t)now->tm_hour,                 // 时
        (uint8_t)now->tm_min,                  // 分
        (uint8_t)now->tm_sec,                  // 秒
        (uint8_t)now->tm_wday,                 // 星期（0-6）
        ((uint8_t)lunar.year - 2020), // 农历年（简化）
        ((uint8_t)l_month - 1), // 农历月（简化）
        (uint8_t)lunar.day, // 农历日（简化）
    };

    // 写入时间数据（true表示等待响应）
    if (pChar->writeValue(timeBuf, 13, true)) {
        Serial.println("✅ 时间同步成功");
        return true;
    } else {
        Serial.println("❌ 时间写入失败");
        return false;
    }
}

// 连接设备并执行时间同步
bool connectAndSync() {
    NimBLEClient* pClient = nullptr;
    std::string devAddr = advDevice->getAddress().toString();
    const int MAX_RETRIES = 3;  // 最大重试次数
    int retries = 0;
    
    Serial.printf("⏳ 发现设备 %s，延迟2s后开始连接...\n", devAddr.c_str());
    delay(2000);
    
    // 连接重试机制
    while (retries < MAX_RETRIES) {
        if (retries > 0) {
            Serial.printf("🔄 尝试第 %d 次重连...\n", retries + 1);
            delay(1000);  // 重试间隔1秒
        }

        // 清理之前的连接
        if (pClient) {
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
            delay(100);  // 等待资源释放
        }

        // 客户端管理
        if (NimBLEDevice::getCreatedClientCount()) {
            delay(50);
            pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
            delay(50);
            if (pClient) {
                if (!pClient->connect(advDevice, false)) {
                    Serial.println("❌ 重用客户端连接失败");
                } else {
                    Serial.println("✅ 重用客户端连接成功");
                    break;  // 连接成功，跳出重试循环
                }
            } else {
                pClient = NimBLEDevice::getDisconnectedClient();
            }
        }

        if (!pClient) {
            if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS) {
                Serial.println("❌ 已达最大连接数，无法创建新客户端");
                return false;
            }

            pClient = NimBLEDevice::createClient();
            Serial.println("📌 创建新客户端");
            
            // 设置连接参数
            pClient->setClientCallbacks(&clientCallbacks, false);
            pClient->setConnectionParams(6, 12, 0, 100);  // 使用更快的连接间隔
            pClient->setConnectTimeout(3000 + retries * 1000);  // 递增超时时间


            if (!pClient->connect(advDevice)) {
                NimBLEDevice::deleteClient(pClient);
                Serial.println("❌ 新客户端连接失败");
                delay(100);
                NimBLEDevice::deleteClient(pClient);
                pClient = nullptr;  // 重置指针
            } else {
                Serial.println("✅ 连接成功！");
                // 连接成功后立即设置连接参数
                pClient->updateConnParams(12, 12, 0, 300);
                delay(200);  // 等待参数更新
                break;  // 连接成功，跳出重试循环
            }
        }
        
        retries++;
    }
    
    // 检查是否成功连接
    if (!pClient || !pClient->isConnected()) {
        if (pClient) {
            NimBLEDevice::deleteClient(pClient);
        }
        Serial.println("❌ 多次尝试后仍连接失败");
        return false;
    }

    // 执行时间同步
    bool syncSuccess = false;
    try {
        syncSuccess = syncTimeToDevice(pClient);
    } catch (...) {
        Serial.println("❌ 同步过程发生异常");
    }

    if (syncSuccess) {
        recentConnections[devAddr] = millis();
    }

    // 断开连接并释放资源
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
        delay(100);  // 等待断开完成
    }
    NimBLEDevice::deleteClient(pClient);
    Serial.println("📌 同步完成，断开连接");

    return syncSuccess;
}

void setup() {
    Serial.begin(115200);
    while (!Serial) {}  // 等待串口就绪
    Serial.println("====================================");
    Serial.println("NimBLE 对时客户端启动中...");

    // 加载保存的WiFi配置
    loadWiFiConfig();
    
    // 尝试连接WiFi
    bool wifiConnected = connectWiFi();
    
    // 如果WiFi连接失败，启动配网模式
    if (!wifiConnected) {
        startConfigAP();
    } else {
        // WiFi连接成功，同步NTP时间
        syncNtpTime();
    }

    // 初始化BLE
    NimBLEDevice::init("DLG-Time-Sync");
    NimBLEDevice::setPower(3);

    // 配置扫描参数
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(&scanCallbacks, false);
    pScan->setInterval(80);
    pScan->setWindow(40);
    pScan->setActiveScan(true);

    // 开始扫描
    pScan->start(scanTimeMs);
    Serial.println("开始扫描目标设备...");
    Serial.println("====================================");
}

void loop() {
    // 如果处于配网模式，处理HTTP请求
    if (WiFi.getMode() & WIFI_AP) {
        server.handleClient();
        delay(10);
        return;
    }

    delay(10);  // 轻微延时，降低CPU占用

    // 当触发连接标志时，执行连接和同步
    if (doConnect) {
        doConnect = false;  // 重置标志
        Serial.println("\n====================================");
        Serial.println("开始连接目标设备...");
        
        if (connectAndSync()) {
            Serial.println("时间同步流程完成");
        } else {
            Serial.println("时间同步流程失败");
        }

        // 重启扫描（继续寻找其他设备）
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
        Serial.println("====================================");
    }
}