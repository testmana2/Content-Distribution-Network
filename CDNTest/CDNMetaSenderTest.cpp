#include "../CDN/CDNMetaSender.h"
#include "cpprest/http_client.h"

using namespace std;
using namespace web;
using namespace http;
using namespace utility;
using namespace http::client;

void simpleTest() {
	utility::string_t port = U("5000");
	utility::string_t address = U("http://localhost:");

	CDNMetaSender sender("http://localhost:5000/");

	Address la_client(make_pair(34.05, -118.44), "0.0.0.0");
	Address sf(make_pair(37.77, -122.42), "1.1.1.1");
	Address seattle(make_pair(47.61, -122.33), "2.2.2.2");
	Address bahama(make_pair(25.03, -77.40), "3.3.3.3");
	Address northkorea(make_pair(40.34, 127.51), "4.4.4.4");
	Address austin_fss(make_pair(30.27, -97.74), "255.255.255.255");

	sender.sendCacheDeleteMsg("a", sf);
	sender.sendCacheDeleteMsg("q", sf);
	sender.sendCacheDeleteMsg("b", sf);
}

int main() {
	simpleTest();
}
