#include <iostream>
#include <map>
#include <fstream>
#include <string>

#include "v2v/v2v.hpp"


using namespace std;
int main(int /*argc*/, char** /*argv*/) {
   
    shared_ptr<V2VService> v2vService = make_shared<V2VService>("192.168.43.212", "7");


		
		using namespace std::chrono_literals;
    while (true) {        
        // delay
        std::this_thread::sleep_for(500ms);
    		


	}
}
