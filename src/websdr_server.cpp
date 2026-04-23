/*
 * WebSDR Server - Multi-band SDR server for RX888
 * Reads IQ data from FIFO files provided by radiod
 * Supports sample rates from 192 kHz to 1600 kHz
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <fftw3.h>

// Constants
const int MAX_USERS = 200;
const int DEFAULT_PORT = 80;
const int BUFFER_SIZE = 65536;
const int FFT_SIZE = 4096;
const int WATERFALL_WIDTH = 512;
const int WATERFALL_HEIGHT = 256;

// Complex number type
using IQSample = std::complex<float>;

// Band configuration structure
struct BandConfig {
    std::string name;
    std::string device;  // FIFO path
    int sampleRate;
    float centerFreq;
    float gain;
    bool swapIQ;
    int extraZoom;
    int noiseBlanker;
    std::string antenna;
    bool enabled;
    
    BandConfig() : sampleRate(768000), centerFreq(7000), gain(-20), 
                   swapIQ(false), extraZoom(0), noiseBlanker(0), enabled(true) {}
};

// Client connection structure
struct Client {
    int socket;
    std::string band;
    float frequency;
    std::string mode;
    int waterfallSpeed;
    bool audioEnabled;
    bool active;
    
    Client() : socket(-1), frequency(7000), mode("lsb"), 
               waterfallSpeed(50), audioEnabled(true), active(false) {}
};

// Global variables
std::atomic<bool> g_running(true);
std::mutex g_clientsMutex;
std::map<int, Client> g_clients;
std::map<std::string, BandConfig> g_bands;
std::string g_initialFreq = "7000";
std::string g_initialMode = "lsb";
int g_tcpPort = DEFAULT_PORT;
int g_maxUsers = MAX_USERS;

// Signal handler
void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    g_running = false;
}

// Parse configuration file
bool parseConfig(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open config file: " << filename << std::endl;
        return false;
    }
    
    std::string line;
    std::string currentBand;
    
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        std::istringstream iss(line);
        std::string keyword;
        iss >> keyword;
        
        if (keyword == "tcpport") {
            iss >> g_tcpPort;
        } else if (keyword == "maxusers") {
            iss >> g_maxUsers;
        } else if (keyword == "initial") {
            iss >> g_initialFreq >> g_initialMode;
        } else if (keyword == "band") {
            iss >> currentBand;
            g_bands[currentBand] = BandConfig();
            g_bands[currentBand].name = currentBand;
        } else if (keyword == "device" && !currentBand.empty()) {
            iss >> g_bands[currentBand].device;
        } else if (keyword == "samplerate" && !currentBand.empty()) {
            iss >> g_bands[currentBand].sampleRate;
        } else if (keyword == "centerfreq" && !currentBand.empty()) {
            iss >> g_bands[currentBand].centerFreq;
        } else if (keyword == "gain" && !currentBand.empty()) {
            iss >> g_bands[currentBand].gain;
        } else if (keyword == "swapiq" && !currentBand.empty()) {
            g_bands[currentBand].swapIQ = true;
        } else if (keyword == "extrazoom" && !currentBand.empty()) {
            iss >> g_bands[currentBand].extraZoom;
        } else if (keyword == "noiseblanker" && !currentBand.empty()) {
            iss >> g_bands[currentBand].noiseBlanker;
        } else if (keyword == "antenna" && !currentBand.empty()) {
            std::string antenna;
            std::getline(iss >> std::ws, antenna);
            g_bands[currentBand].antenna = antenna;
        }
    }
    
    file.close();
    std::cout << "Configuration loaded: " << g_bands.size() << " bands configured" << std::endl;
    return true;
}

// Band reader thread - reads IQ data from FIFO
class BandReader {
private:
    BandConfig& config;
    std::vector<IQSample> buffer;
    std::mutex bufferMutex;
    std::condition_variable bufferCV;
    std::thread readerThread;
    std::atomic<bool> running;
    int fifoFd;
    
public:
    BandReader(BandConfig& cfg) : config(cfg), running(false), fifoFd(-1) {
        buffer.resize(BUFFER_SIZE);
    }
    
    ~BandReader() {
        stop();
    }
    
    bool start() {
        if (config.device.empty()) {
            std::cerr << "No device configured for band " << config.name << std::endl;
            return false;
        }
        
        running = true;
        readerThread = std::thread(&BandReader::readerLoop, this);
        return true;
    }
    
    void stop() {
        running = false;
        if (readerThread.joinable()) {
            readerThread.join();
        }
        if (fifoFd >= 0) {
            close(fifoFd);
            fifoFd = -1;
        }
    }
    
    void readerLoop() {
        std::cout << "Starting reader for band " << config.name 
                  << " on " << config.device << std::endl;
        
        while (running) {
            // Open FIFO
            fifoFd = open(config.device.c_str(), O_RDONLY | O_NONBLOCK);
            if (fifoFd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            std::cout << "Connected to FIFO for band " << config.name << std::endl;
            
            // Read IQ data
            char readBuffer[BUFFER_SIZE * sizeof(IQSample)];
            
            while (running && fifoFd >= 0) {
                ssize_t bytesRead = read(fifoFd, readBuffer, sizeof(readBuffer));
                
                if (bytesRead > 0) {
                    // Convert to IQ samples
                    int numSamples = bytesRead / sizeof(IQSample);
                    
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    
                    for (int i = 0; i < numSamples; i++) {
                        float* ptr = reinterpret_cast<float*>(readBuffer + i * sizeof(IQSample));
                        IQSample sample(ptr[0], ptr[1]);
                        
                        if (config.swapIQ) {
                            sample = IQSample(ptr[1], ptr[0]);
                        }
                        
                        // Apply gain
                        float gainFactor = pow(10.0f, config.gain / 20.0f);
                        sample *= gainFactor;
                        
                        buffer[i % BUFFER_SIZE] = sample;
                    }
                    
                    bufferCV.notify_all();
                } else if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "Error reading from FIFO: " << strerror(errno) << std::endl;
                    close(fifoFd);
                    fifoFd = -1;
                    break;
                }
                
                if (bytesRead == 0) {
                    // EOF - writer closed, reopen
                    close(fifoFd);
                    fifoFd = -1;
                    break;
                }
            }
            
            if (fifoFd >= 0) {
                close(fifoFd);
                fifoFd = -1;
            }
        }
    }
    
    bool getSamples(std::vector<IQSample>& outBuffer, int count) {
        std::unique_lock<std::mutex> lock(bufferMutex);
        
        // Wait for data if buffer is empty
        if (buffer.empty()) {
            bufferCV.wait_for(lock, std::chrono::milliseconds(100));
        }
        
        // Copy available samples
        int available = std::min(count, static_cast<int>(buffer.size()));
        outBuffer.assign(buffer.begin(), buffer.begin() + available);
        
        return !outBuffer.empty();
    }
    
    int getSampleRate() const { return config.sampleRate; }
    float getCenterFreq() const { return config.centerFreq; }
};

// HTTP Server class
class WebSDRServer {
private:
    int serverSocket;
    std::vector<std::unique_ptr<BandReader>> bandReaders;
    std::thread acceptThread;
    
public:
    WebSDRServer() : serverSocket(-1) {}
    
    ~WebSDRServer() {
        stop();
    }
    
    bool start() {
        // Create band readers
        for (auto& pair : g_bands) {
            if (pair.second.enabled) {
                auto reader = std::make_unique<BandReader>(pair.second);
                reader->start();
                bandReaders.push_back(std::move(reader));
            }
        }
        
        // Create socket
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket < 0) {
            std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Set socket options
        int opt = 1;
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // Bind
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(g_tcpPort);
        
        if (bind(serverSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Error binding to port " << g_tcpPort << ": " << strerror(errno) << std::endl;
            close(serverSocket);
            return false;
        }
        
        // Listen
        if (listen(serverSocket, 10) < 0) {
            std::cerr << "Error listening: " << strerror(errno) << std::endl;
            close(serverSocket);
            return false;
        }
        
        std::cout << "WebSDR server listening on port " << g_tcpPort << std::endl;
        
        // Start accept thread
        acceptThread = std::thread(&WebSDRServer::acceptLoop, this);
        
        return true;
    }
    
    void stop() {
        g_running = false;
        
        if (serverSocket >= 0) {
            close(serverSocket);
            serverSocket = -1;
        }
        
        if (acceptThread.joinable()) {
            acceptThread.join();
        }
        
        // Stop all band readers
        for (auto& reader : bandReaders) {
            reader->stop();
        }
        bandReaders.clear();
    }
    
    void acceptLoop() {
        while (g_running) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            
            int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);
            
            if (clientSocket < 0) {
                if (g_running) {
                    std::cerr << "Error accepting connection: " << strerror(errno) << std::endl;
                }
                continue;
            }
            
            std::cout << "New connection from " << inet_ntoa(clientAddr.sin_addr) << std::endl;
            
            // Check user limit
            {
                std::lock_guard<std::mutex> lock(g_clientsMutex);
                if (static_cast<int>(g_clients.size()) >= g_maxUsers) {
                    std::string response = "HTTP/1.1 503 Service Unavailable\r\n\r\nServer full";
                    send(clientSocket, response.c_str(), response.length(), 0);
                    close(clientSocket);
                    continue;
                }
            }
            
            // Handle client in new thread
            std::thread(&WebSDRServer::handleClient, this, clientSocket).detach();
        }
    }
    
    void handleClient(int clientSocket) {
        Client client;
        client.socket = clientSocket;
        client.active = true;
        client.frequency = std::stof(g_initialFreq);
        client.mode = g_initialMode;
        
        // Add to clients map
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            g_clients[clientSocket] = client;
        }
        
        char buffer[4096];
        bool keepAlive = true;
        
        while (g_running && keepAlive && client.active) {
            int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
            
            if (bytesRead <= 0) {
                break;
            }
            
            buffer[bytesRead] = '\0';
            
            // Parse HTTP request
            std::string request(buffer);
            std::istringstream iss(request);
            std::string method, path, protocol;
            iss >> method >> path >> protocol;
            
            std::cout << "Request: " << method << " " << path << std::endl;
            
            // Route request
            if (method == "GET") {
                keepAlive = handleGetRequest(clientSocket, path, client);
            } else if (method == "POST") {
                keepAlive = handlePostRequest(clientSocket, request, client);
            } else {
                sendErrorResponse(clientSocket, 405, "Method Not Allowed");
            }
        }
        
        // Cleanup
        close(clientSocket);
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            g_clients.erase(clientSocket);
        }
        std::cout << "Client disconnected" << std::endl;
    }
    
    bool handleGetRequest(int clientSocket, const std::string& path, Client& client) {
        // Serve static files from web/pub2
        std::string filePath;
        
        if (path == "/" || path.empty()) {
            filePath = "web/pub2/index.html";
        } else if (path.find("..") != std::string::npos) {
            sendErrorResponse(clientSocket, 403, "Forbidden");
            return false;
        } else {
            filePath = "web/pub2" + path;
        }
        
        // Check if file exists
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            // Try pub directory
            filePath = "pub" + path;
            file.open(filePath, std::ios::binary);
        }
        
        if (file.is_open()) {
            // Get file size
            file.seekg(0, std::ios::end);
            size_t fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            
            // Determine content type
            std::string contentType = "text/html";
            if (path.find(".css") != std::string::npos) contentType = "text/css";
            else if (path.find(".js") != std::string::npos) contentType = "application/javascript";
            else if (path.find(".png") != std::string::npos) contentType = "image/png";
            else if (path.find(".jpg") != std::string::npos) contentType = "image/jpeg";
            else if (path.find(".html") != std::string::npos) contentType = "text/html";
            
            // Send response
            std::ostringstream header;
            header << "HTTP/1.1 200 OK\r\n";
            header << "Content-Type: " << contentType << "\r\n";
            header << "Content-Length: " << fileSize << "\r\n";
            header << "Connection: keep-alive\r\n";
            header << "\r\n";
            
            send(clientSocket, header.str().c_str(), header.str().length(), 0);
            
            // Send file content
            char fileBuffer[4096];
            while (file.read(fileBuffer, sizeof(fileBuffer))) {
                send(clientSocket, fileBuffer, file.gcount(), 0);
            }
            
            return true;
        }
        
        // API endpoints
        if (path.find("/getwaterfall?") != std::string::npos) {
            return sendWaterfallData(clientSocket, client);
        } else if (path.find("/getaudio?") != std::string::npos) {
            return sendAudioData(clientSocket, client);
        } else if (path.find("/status") != std::string::npos) {
            return sendStatus(clientSocket);
        }
        
        sendErrorResponse(clientSocket, 404, "Not Found");
        return false;
    }
    
    bool handlePostRequest(int clientSocket, const std::string& request, Client& client) {
        // Handle parameter updates
        size_t bodyPos = request.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            std::string body = request.substr(bodyPos + 4);
            
            // Parse parameters
            if (body.find("frequency=") != std::string::npos) {
                size_t pos = body.find("frequency=");
                client.frequency = std::stof(body.substr(pos + 10));
            }
            if (body.find("mode=") != std::string::npos) {
                size_t pos = body.find("mode=");
                size_t end = body.find("&", pos);
                if (end == std::string::npos) end = body.length();
                client.mode = body.substr(pos + 5, end - pos - 5);
            }
            if (body.find("band=") != std::string::npos) {
                size_t pos = body.find("band=");
                size_t end = body.find("&", pos);
                if (end == std::string::npos) end = body.length();
                client.band = body.substr(pos + 5, end - pos - 5);
            }
        }
        
        send(clientSocket, "HTTP/1.1 200 OK\r\n\r\n", 20, 0);
        return true;
    }
    
    bool sendWaterfallData(int clientSocket, Client& client) {
        // Generate waterfall data from IQ samples
        // This is a simplified implementation
        
        std::vector<uint8_t> waterfallData(WATERFALL_WIDTH * WATERFALL_HEIGHT);
        
        // Fill with dummy data for now
        for (size_t i = 0; i < waterfallData.size(); i++) {
            waterfallData[i] = rand() % 256;
        }
        
        // Send response
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n";
        header << "Content-Type: application/octet-stream\r\n";
        header << "Content-Length: " << waterfallData.size() << "\r\n";
        header << "\r\n";
        
        send(clientSocket, header.str().c_str(), header.str().length(), 0);
        send(clientSocket, waterfallData.data(), waterfallData.size(), 0);
        
        return true;
    }
    
    bool sendAudioData(int clientSocket, Client& client) {
        // Send audio data (demodulated)
        // Simplified implementation
        
        std::vector<int16_t> audioData(4096);
        
        // Generate dummy audio
        for (size_t i = 0; i < audioData.size(); i++) {
            audioData[i] = (rand() % 65535) - 32768;
        }
        
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n";
        header << "Content-Type: application/octet-stream\r\n";
        header << "Content-Length: " << (audioData.size() * sizeof(int16_t)) << "\r\n";
        header << "\r\n";
        
        send(clientSocket, header.str().c_str(), header.str().length(), 0);
        send(clientSocket, audioData.data(), audioData.size() * sizeof(int16_t), 0);
        
        return true;
    }
    
    bool sendStatus(int clientSocket) {
        std::ostringstream json;
        json << "{";
        json << "\"bands\": [";
        
        bool first = true;
        for (const auto& pair : g_bands) {
            if (!first) json << ",";
            first = false;
            json << "{\"name\":\"" << pair.first << "\",";
            json << "\"centerFreq\":" << pair.second.centerFreq << ",";
            json << "\"sampleRate\":" << pair.second.sampleRate << ",";
            json << "\"enabled\":" << (pair.second.enabled ? "true" : "false") << "}";
        }
        
        json << "],\"users\":" << g_clients.size() << "}";
        
        std::string response = json.str();
        
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n";
        header << "Content-Type: application/json\r\n";
        header << "Content-Length: " << response.length() << "\r\n";
        header << "\r\n";
        
        send(clientSocket, header.str().c_str(), header.str().length(), 0);
        send(clientSocket, response.c_str(), response.length(), 0);
        
        return true;
    }
    
    void sendErrorResponse(int clientSocket, int code, const std::string& message) {
        std::ostringstream response;
        response << "HTTP/1.1 " << code << " " << message << "\r\n";
        response << "Content-Type: text/html\r\n";
        response << "Content-Length: " << message.length() << "\r\n";
        response << "\r\n";
        response << message;
        
        send(clientSocket, response.str().c_str(), response.str().length(), 0);
    }
};

// Main function
int main(int argc, char* argv[]) {
    std::cout << "WebSDR Server starting..." << std::endl;
    std::cout << "Multi-band SDR server for RX888" << std::endl;
    
    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Parse configuration
    std::string configFile = "cfg/websdr.cfg";
    if (argc > 1) {
        configFile = argv[1];
    }
    
    if (!parseConfig(configFile)) {
        std::cerr << "Failed to load configuration" << std::endl;
        return 1;
    }
    
    // Print band information
    std::cout << "\nConfigured bands:" << std::endl;
    for (const auto& pair : g_bands) {
        std::cout << "  " << pair.first << ": " 
                  << pair.second.centerFreq << " kHz, "
                  << pair.second.sampleRate << " sps, FIFO: "
                  << pair.second.device << std::endl;
    }
    std::cout << std::endl;
    
    // Initialize FFTW
    fftwf_init_threads();
    
    // Start server
    WebSDRServer server;
    if (!server.start()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    
    std::cout << "Server running. Press Ctrl+C to stop." << std::endl;
    
    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Print status
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            std::cout << "Active users: " << g_clients.size() << "/" << g_maxUsers << std::endl;
        }
    }
    
    std::cout << "Shutting down..." << std::endl;
    server.stop();
    fftwf_cleanup_threads();
    
    return 0;
}
