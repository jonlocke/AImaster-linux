#include "tool_plugins.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace {

class ToolPlugin {
public:
    virtual ~ToolPlugin() = default;
    virtual std::string name() const = 0;
    virtual bool enabled(const AppConfig& config) const = 0;
    virtual Json::Value definition() const = 0;
    virtual PluginResult invoke(const Json::Value& arguments, const AppConfig& config) const = 0;
};

size_t write_to_string(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    std::string* out = static_cast<std::string*>(userp);
    out->append(static_cast<const char*>(contents), total);
    return total;
}

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool parse_json_string(const std::string& input, Json::Value& out, std::string& error) {
    Json::CharReaderBuilder reader;
    std::istringstream ss(input);
    return Json::parseFromStream(reader, ss, &out, &error);
}

std::string json_stringify(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

PluginResult failure(const std::string& message) {
    PluginResult result;
    result.success = false;
    result.error_message = message;
    Json::Value payload(Json::objectValue);
    payload["ok"] = false;
    payload["error"] = message;
    result.content = json_stringify(payload);
    return result;
}

std::string weather_code_description(int code) {
    switch (code) {
        case 0: return "clear sky";
        case 1: return "mainly clear";
        case 2: return "partly cloudy";
        case 3: return "overcast";
        case 45:
        case 48: return "fog";
        case 51:
        case 53:
        case 55: return "drizzle";
        case 56:
        case 57: return "freezing drizzle";
        case 61:
        case 63:
        case 65: return "rain";
        case 66:
        case 67: return "freezing rain";
        case 71:
        case 73:
        case 75:
        case 77: return "snow";
        case 80:
        case 81:
        case 82: return "rain showers";
        case 85:
        case 86: return "snow showers";
        case 95: return "thunderstorm";
        case 96:
        case 99: return "thunderstorm with hail";
        default: return "unknown";
    }
}

bool http_get_json(const std::string& url,
                   long timeout_seconds,
                   Json::Value& out_json,
                   std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl init failed";
        return false;
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AImaster/1.0");

    const CURLcode res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        error = std::string("request failed: ") + curl_easy_strerror(res);
        return false;
    }
    if (http_status >= 400) {
        error = "weather API returned HTTP " + std::to_string(http_status);
        return false;
    }
    if (!parse_json_string(response, out_json, error)) {
        error = "invalid JSON from weather API: " + error;
        return false;
    }
    return true;
}

std::string encode_url_param(CURL* curl, const std::string& value) {
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!encoded) return "";
    std::string out(encoded);
    curl_free(encoded);
    return out;
}

class WeatherPlugin : public ToolPlugin {
public:
    std::string name() const override { return "get_weather"; }

    bool enabled(const AppConfig& config) const override {
        return config.weather_plugin_enabled;
    }

    Json::Value definition() const override {
        Json::Value tool(Json::objectValue);
        tool["type"] = "function";
        tool["function"] = Json::Value(Json::objectValue);
        tool["function"]["name"] = name();
        tool["function"]["description"] = "Resolve a real-world place name and return current weather plus a short forecast.";
        tool["function"]["parameters"] = Json::Value(Json::objectValue);
        tool["function"]["parameters"]["type"] = "object";
        tool["function"]["parameters"]["properties"] = Json::Value(Json::objectValue);
        tool["function"]["parameters"]["properties"]["location"] = Json::Value(Json::objectValue);
        tool["function"]["parameters"]["properties"]["location"]["type"] = "string";
        tool["function"]["parameters"]["properties"]["location"]["description"] = "City, town, region, or postal-style location to look up.";
        tool["function"]["parameters"]["properties"]["days"] = Json::Value(Json::objectValue);
        tool["function"]["parameters"]["properties"]["days"]["type"] = "integer";
        tool["function"]["parameters"]["properties"]["days"]["description"] = "Forecast days to include, between 1 and 3.";
        tool["function"]["parameters"]["properties"]["temperature_unit"] = Json::Value(Json::objectValue);
        tool["function"]["parameters"]["properties"]["temperature_unit"]["type"] = "string";
        tool["function"]["parameters"]["properties"]["temperature_unit"]["enum"] = Json::arrayValue;
        tool["function"]["parameters"]["properties"]["temperature_unit"]["enum"].append("celsius");
        tool["function"]["parameters"]["properties"]["temperature_unit"]["enum"].append("fahrenheit");
        tool["function"]["parameters"]["required"] = Json::arrayValue;
        tool["function"]["parameters"]["required"].append("location");
        return tool;
    }

    PluginResult invoke(const Json::Value& arguments, const AppConfig& config) const override {
        const std::string location = trim_copy(arguments.get("location", "").asString());
        if (location.empty()) return failure("location is required");

        int days = arguments.get("days", 1).asInt();
        if (days < 1) days = 1;
        if (days > 3) days = 3;

        std::string temperature_unit = trim_copy(arguments.get("temperature_unit", "celsius").asString());
        if (temperature_unit != "celsius" && temperature_unit != "fahrenheit") temperature_unit = "celsius";

        CURL* curl = curl_easy_init();
        if (!curl) return failure("curl init failed");
        const std::string encoded_location = encode_url_param(curl, location);
        curl_easy_cleanup(curl);
        if (encoded_location.empty()) return failure("failed to encode location");

        Json::Value geocode_json;
        std::string error;
        std::string geocode_url = config.weather_geocoding_url +
            "?name=" + encoded_location +
            "&count=1&language=en&format=json";
        if (!http_get_json(geocode_url, config.weather_timeout_seconds, geocode_json, error)) {
            return failure(error);
        }

        const Json::Value& results = geocode_json["results"];
        if (!results.isArray() || results.empty()) return failure("location not found");

        const Json::Value& place = results[0];
        const double latitude = place.get("latitude", 0.0).asDouble();
        const double longitude = place.get("longitude", 0.0).asDouble();

        std::ostringstream forecast_url;
        forecast_url << config.weather_forecast_url
                     << "?latitude=" << latitude
                     << "&longitude=" << longitude
                     << "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
                     << "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                     << "&forecast_days=" << days
                     << "&timezone=auto"
                     << "&temperature_unit=" << temperature_unit
                     << "&wind_speed_unit=kmh";

        Json::Value forecast_json;
        error.clear();
        if (!http_get_json(forecast_url.str(), config.weather_timeout_seconds, forecast_json, error)) {
            return failure(error);
        }

        Json::Value payload(Json::objectValue);
        payload["ok"] = true;
        payload["source"] = "open-meteo";
        payload["requested_location"] = location;
        payload["resolved_location"] = place.get("name", location).asString();
        if (place.isMember("admin1")) payload["region"] = place["admin1"].asString();
        if (place.isMember("country")) payload["country"] = place["country"].asString();
        payload["latitude"] = latitude;
        payload["longitude"] = longitude;
        payload["timezone"] = forecast_json.get("timezone", "").asString();

        const Json::Value& current = forecast_json["current"];
        Json::Value current_out(Json::objectValue);
        current_out["temperature"] = current.get("temperature_2m", 0.0);
        current_out["apparent_temperature"] = current.get("apparent_temperature", 0.0);
        current_out["relative_humidity"] = current.get("relative_humidity_2m", 0);
        current_out["wind_speed"] = current.get("wind_speed_10m", 0.0);
        current_out["weather_code"] = current.get("weather_code", -1);
        current_out["conditions"] = weather_code_description(current.get("weather_code", -1).asInt());
        current_out["temperature_unit"] = forecast_json.get("current_units", Json::Value(Json::objectValue)).get("temperature_2m", "").asString();
        current_out["wind_speed_unit"] = forecast_json.get("current_units", Json::Value(Json::objectValue)).get("wind_speed_10m", "").asString();
        payload["current"] = current_out;

        payload["forecast"] = Json::arrayValue;
        const Json::Value& daily = forecast_json["daily"];
        const Json::Value& dates = daily["time"];
        const Json::Value& max_temps = daily["temperature_2m_max"];
        const Json::Value& min_temps = daily["temperature_2m_min"];
        const Json::Value& codes = daily["weather_code"];
        for (Json::ArrayIndex i = 0; i < dates.size(); ++i) {
            Json::Value day(Json::objectValue);
            day["date"] = dates[i];
            day["max_temperature"] = i < max_temps.size() ? max_temps[i] : Json::Value();
            day["min_temperature"] = i < min_temps.size() ? min_temps[i] : Json::Value();
            const int code = i < codes.size() ? codes[i].asInt() : -1;
            day["weather_code"] = code;
            day["conditions"] = weather_code_description(code);
            payload["forecast"].append(day);
        }

        PluginResult result;
        result.success = true;
        result.content = json_stringify(payload);
        return result;
    }
};

std::vector<const ToolPlugin*>& plugins() {
    static WeatherPlugin weather_plugin;
    static std::vector<const ToolPlugin*> registry = {&weather_plugin};
    return registry;
}

const ToolPlugin* find_plugin(const std::string& name) {
    for (const ToolPlugin* plugin : plugins()) {
        if (plugin->name() == name) return plugin;
    }
    return nullptr;
}

Json::Value parse_invocation_arguments(const Json::Value& value) {
    if (value.isObject()) return value;
    if (!value.isString()) return Json::Value(Json::objectValue);

    Json::Value parsed;
    std::string error;
    if (parse_json_string(value.asString(), parsed, error) && parsed.isObject()) return parsed;
    return Json::Value(Json::objectValue);
}

} // namespace

Json::Value buildRegisteredToolDefinitions(const AppConfig& config) {
    Json::Value tools(Json::arrayValue);
    for (const ToolPlugin* plugin : plugins()) {
        if (plugin->enabled(config)) tools.append(plugin->definition());
    }
    return tools;
}

std::vector<PluginInvocation> extractPluginInvocations(const Json::Value& assistant_message) {
    std::vector<PluginInvocation> invocations;

    if (assistant_message.isMember("tool_calls") && assistant_message["tool_calls"].isArray()) {
        for (const auto& item : assistant_message["tool_calls"]) {
            PluginInvocation invocation;
            invocation.id = item.get("id", "").asString();
            if (item.isMember("function") && item["function"].isObject()) {
                invocation.name = item["function"].get("name", "").asString();
                invocation.arguments = parse_invocation_arguments(item["function"]["arguments"]);
            } else {
                invocation.name = item.get("name", "").asString();
                invocation.arguments = parse_invocation_arguments(item["arguments"]);
            }
            if (!invocation.name.empty()) invocations.push_back(invocation);
        }
    } else if (assistant_message.isMember("function_call") && assistant_message["function_call"].isObject()) {
        PluginInvocation invocation;
        invocation.name = assistant_message["function_call"].get("name", "").asString();
        invocation.arguments = parse_invocation_arguments(assistant_message["function_call"]["arguments"]);
        if (!invocation.name.empty()) invocations.push_back(invocation);
    }

    return invocations;
}

PluginResult executePluginInvocation(const PluginInvocation& invocation, const AppConfig& config) {
    const ToolPlugin* plugin = find_plugin(invocation.name);
    if (!plugin) return failure("unknown tool: " + invocation.name);
    if (!plugin->enabled(config)) return failure("tool disabled: " + invocation.name);
    return plugin->invoke(invocation.arguments, config);
}

Json::Value buildPluginResultMessage(const PluginInvocation& invocation, const PluginResult& result) {
    Json::Value message(Json::objectValue);
    message["role"] = "tool";
    message["name"] = invocation.name;
    message["tool_name"] = invocation.name;
    if (!invocation.id.empty()) message["tool_call_id"] = invocation.id;
    message["content"] = result.content;
    return message;
}
