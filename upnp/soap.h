#pragma once
#include "xml.h"
#include <utility>
namespace upnp {
using SoapArguments = std::vector<std::pair<std::string, std::string>>;
std::string soapBody(const std::string& service, const std::string& action, const SoapArguments& args);
std::string streamDidl(const std::string& url, const std::string& title, const std::string& art,
                       const std::string& artist = {}, const std::string& album = {});
struct SoapResult {
    bool ok = false;
    XmlNode response;
    std::string faultCode, faultDescription;
};
SoapResult parseSoap(const std::string& body, const std::string& action);
}
