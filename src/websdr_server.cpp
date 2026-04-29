/*
WebSDR Server - Multi-band SDR server for RX888
Reads IQ data from FIFO files provided by radiod
Supports sample rates from 192 kHz to 1600 kHz
*/
#include <atomic>
#include <complex>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fftw3.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>

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

// Band reader thread - reads IQ data from FIFO and computes Waterfall
class BandReader {
private:
    BandConfig& config;
    std::vector<IQSample> buffer;
    std::mutex bufferMutex;
    std::condition_variable bufferCV;
    std::thread readerThread;
    std::atomic<bool> running;
    int fifoFd;

    // Waterfall & FFT members
    std::deque<std::vector<uint8_t>> waterfallHistory;
    std::mutex wfMutex;
    fftwf_plan fftPlan;
    fftwf_complex *fftIn, *fftOut;

public:
    BandReader(BandConfig& cfg) : config(cfg), running(false), fifoFd(-1), fftPlan(nullptr), fftIn(nullptr), fftOut(nullptr) {
        buffer.resize(BUFFER_SIZE);
    }
    
    ~BandReader() { 
        stop(); 
        if (fftPlan) fftwf_destroy_plan(fftPlan);
        if (fftIn) fftwf_free(fftIn);
        if (fftOut) fftwf_free(fftOut);
    }

    bool start() {
        if (config.device.empty()) {
            std::cerr << "No device configured for band " << config.name << std::endl;
            return false;
        }
        
        // Initialize FFT
        fftIn = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * FFT_SIZE);
        fftOut = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * FFT_SIZE);
        fftPlan = fftwf_plan_dft_1d(FFT_SIZE, fftIn, fftOut, FFTW_FORWARD, FFTW_ESTIMATE);

        running = true;
        readerThread = std::thread(&BandReader::readerLoop, this);
        return true;
    }

    void stop() {
        running = false;
        if (readerThread.joinable()) readerThread.join();
        if (fifoFd >= 0) {
            close(fifoFd);
            fifoFd = -1;
        }
    }

    std::string getName() const { return config.name; }

    // Get current waterfall snapshot
    std::vector<uint8_t> getWaterfallSnapshot() {
        std::lock_guard<std::mutex> lock(wfMutex);
        std::vector<uint8_t> result;
        for (const auto& row : waterfallHistory) {
            result.insert(result.end(), row.begin(), row.end());
        }
        return result;
    }

    void readerLoop() {
        std::cout << "Starting reader for band " << config.name << " on " << config.device << std::endl;
        int reconnectDelay = 100;
        const int MAX_RECONNECT_DELAY = 2000;

        while (running) {
            fifoFd = open(config.device.c_str(), O_RDONLY);
            if (fifoFd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelay));
                reconnectDelay = std::min(reconnectDelay * 2, MAX_RECONNECT_DELAY);
                continue;
            }
            reconnectDelay = 100;
            std::cout << "Connected to FIFO for band " << config.name << std::endl;

            char readBuffer[BUFFER_SIZE * sizeof(IQSample)];
            bool dataReceived = false;

            while (running && fifoFd >= 0) {
                ssize_t bytesRead = read(fifoFd, readBuffer, sizeof(readBuffer));
                
                if (bytesRead > 0) {
                    dataReceived = true;
                    
                    // 1. Store raw samples for audio clients
                    {
                        std::lock_guard<std::mutex> lock(bufferMutex);
                        buffer.assign(reinterpret_cast<IQSample*>(readBuffer), 
                                      reinterpret_cast<IQSample*>(readBuffer) + bytesRead / sizeof(IQSample));
                        bufferCV.notify_all();
                    }

                    // 2. Process Waterfall (Compute FFT of the first chunk)
                    if ((size_t)bytesRead >= FFT_SIZE * sizeof(IQSample)) {
                        memcpy(fftIn, readBuffer, FFT_SIZE * sizeof(IQSample));
                        fftwf_execute(fftPlan);

                        std::vector<uint8_t> row(WATERFALL_WIDTH);
                        float maxPower = 0.0f;
                        std::vector<float> powerSpectrum(WATERFALL_WIDTH, 0.0f);

                        for (int i = 0; i < WATERFALL_WIDTH; ++i) {
                            float re = fftOut[i][0];
                            float im = fftOut[i][1];
                            float power = re * re + im * im;
                            powerSpectrum[i] = power;
                            if (power > maxPower) maxPower = power;
                        }

                        for (int i = 0; i < WATERFALL_WIDTH; ++i) {
                            if (maxPower > 0) {
                                float db = 10.0f * log10f(powerSpectrum[i] / maxPower + 1e-9f); 
                                int val = (int)((db + 40.0f) / 40.0f * 255.0f);
                                if (val < 0) val = 0;
                                if (val > 255) val = 255;
                                row[i] = (uint8_t)val;
                            } else {
                                row[i] = 0;
                            }
                        }

                        {
                            std::lock_guard<std::mutex> lock(wfMutex);
                            waterfallHistory.push_back(row);
                            if (waterfallHistory.size() > WATERFALL_HEIGHT) {
                                waterfallHistory.pop_front();
                            }
                        }
                    }

                } else if (bytesRead < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    std::cerr << "Error reading FIFO: " << strerror(errno) << std::endl;
                    break;
                }

                if (bytesRead == 0) {
                    if (dataReceived) std::cout << "FIFO closed by writer, reconnecting..." << std::endl;
                    break;
                }
            }

            if (fifoFd >= 0) {
                close(fifoFd);
                fifoFd = -1;
            }
            if (!dataReceived) reconnectDelay = std::min(reconnectDelay * 2, MAX_RECONNECT_DELAY);
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelay));
        }
    }

    bool getSamples(std::vector<IQSample>& outBuffer, int count) {
        std::unique_lock<std::mutex> lock(bufferMutex);
        if (buffer.empty()) bufferCV.wait_for(lock, std::chrono::milliseconds(100));
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
    ~WebSDRServer() { stop(); }

    bool start() {
        for (auto& pair : g_bands) {
            if (pair.second.enabled) {
                auto reader = std::make_unique<BandReader>(pair.second);
                reader->start();
                bandReaders.push_back(std::move(reader));
            }
        }
        
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket < 0) {
            std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        int opt = 1;
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
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
        
        if (listen(serverSocket, 10) < 0) {
            std::cerr << "Error listening: " << strerror(errno) << std::endl;
            close(serverSocket);
            return false;
        }
        
        std::cout << "WebSDR server listening on port " << g_tcpPort << std::endl;
        acceptThread = std::thread(&WebSDRServer::acceptLoop, this);
        return true;
    }

    void stop() {
        g_running = false;
        if (serverSocket >= 0) {
            close(serverSocket);
            serverSocket = -1;
        }
        if (acceptThread.joinable()) acceptThread.join();
        for (auto& reader : bandReaders) reader->stop();
        bandReaders.clear();
    }

    void acceptLoop() {
        while (g_running) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);
            
            if (clientSocket < 0) {
                if (g_running) std::cerr << "Error accepting connection: " << strerror(errno) << std::endl;
                continue;
            }
            
            std::cout << "New connection from " << inet_ntoa(clientAddr.sin_addr) << std::endl;
            
            {
                std::lock_guard<std::mutex> lock(g_clientsMutex);
                if (static_cast<int>(g_clients.size()) >= g_maxUsers) {
                    std::string response = "HTTP/1.1 503 Service Unavailable\r\n\r\nServer full";
                    send(clientSocket, response.c_str(), response.length(), 0);
                    close(clientSocket);
                    continue;
                }
            }
            std::thread(&WebSDRServer::handleClient, this, clientSocket).detach();
        }
    }

    void handleClient(int clientSocket) {
        Client client;
        client.socket = clientSocket;
        client.active = true;
        client.frequency = std::stof(g_initialFreq);
        client.mode = g_initialMode;
        
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            g_clients[clientSocket] = client;
        }

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        char buffer[4096];
        bool keepAlive = true;
        
        while (g_running && keepAlive && client.active) {
            int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) break;
            buffer[bytesRead] = '\0';
            
            std::string request(buffer);
            std::istringstream iss(request);
            std::string method, path, protocol;
            iss >> method >> path >> protocol;
            
            if (method == "GET") {
                keepAlive = handleGetRequest(clientSocket, path, client);
            } else if (method == "POST") {
                keepAlive = handlePostRequest(clientSocket, request, client);
            } else {
                sendErrorResponse(clientSocket, 405, "Method Not Allowed");
            }
        }
        
        close(clientSocket);
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            g_clients.erase(clientSocket);
        }
        std::cout << "Client disconnected" << std::endl;
    }

    bool handleGetRequest(int clientSocket, const std::string& path, Client&) {
        std::string filePath;
        
        if (path == "/" || path.empty()) {
            filePath = "web/pub2/index.html";
        } else if (path.find("..") != std::string::npos) {
            sendErrorResponse(clientSocket, 403, "Forbidden");
            return false;
        } else {
            filePath = "web/pub2" + path;
        }
        
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            sendErrorResponse(clientSocket, 404, "Not Found");
            return false;
        }
        
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        
        std::string contentType = "text/html";
        if (path.find(".css") != std::string::npos) contentType = "text/css";
        else if (path.find(".js") != std::string::npos) contentType = "application/javascript";
        else if (path.find(".png") != std::string::npos) contentType = "image/png";
        else if (path.find(".jpg") != std::string::npos) contentType = "image/jpeg";
        else if (path.find(".html") != std::string::npos) contentType = "text/html";
        else if (path.find(".ttf") != std::string::npos) contentType = "font/ttf";
        else if (path.find(".woff") != std::string::npos) contentType = "font/woff";
        else if (path.find(".woff2") != std::string::npos) contentType = "font/woff2";
        else if (path.find(".svg") != std::string::npos) contentType = "image/svg+xml";
        else if (path.find(".ico") != std::string::npos) contentType = "image/x-icon";
        else if (path.find(".json") != std::string::npos) contentType = "application/json";
        
        std::string headers = "HTTP/1.1 200 OK\r\n"
            "Content-Type: " + contentType + "\r\n"
            "Content-Length: " + std::to_string(fileSize) + "\r\n"
            "Connection: close\r\n\r\n";
        
        if (send(clientSocket, headers.c_str(), headers.size(), MSG_NOSIGNAL) < 0) return false;
        
        char fileBuffer[4096];
        size_t sentTotal = 0;
        while (sentTotal < fileSize && file.good()) {
            file.read(fileBuffer, sizeof(fileBuffer));
            size_t toSend = file.gcount();
            if (toSend == 0) break;

            size_t sentNow = 0;
            while (sentNow < toSend) {
                ssize_t n = send(clientSocket, fileBuffer + sentNow, toSend - sentNow, MSG_NOSIGNAL);
                if (n <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    file.close();
                    return false;
                }
                sentNow += n;
            }
            sentTotal += toSend;
        }
        file.close();
        return (sentTotal == fileSize);
    }

    bool handlePostRequest(int clientSocket, const std::string& request, Client& client) {
        size_t bodyPos = request.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            std::string body = request.substr(bodyPos + 4);
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
        send(clientSocket, "HTTP/1.1 200 OK\r\n\r\n", 20, MSG_NOSIGNAL);
        return true;
    }

    bool sendWaterfallData(int clientSocket, Client& client) {
        (void)client;
        BandReader* reader = nullptr;
        for (auto& r : bandReaders) {
            if (r->getName() == client.band) {
                reader = r.get();
                break;
            }
        }
        
        if (!reader && !bandReaders.empty()) reader = bandReaders[0].get();
        if (!reader) {
            sendErrorResponse(clientSocket, 404, "No bands available");
            return false;
        }

        std::vector<uint8_t> data = reader->getWaterfallSnapshot();
        if (data.empty()) data.resize(WATERFALL_WIDTH * WATERFALL_HEIGHT, 0);

        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/octet-stream\r\n"
               << "Content-Length: " << data.size() << "\r\n"
               << "\r\n";
        
        send(clientSocket, header.str().c_str(), header.str().length(), MSG_NOSIGNAL);
        send(clientSocket, data.data(), data.size(), MSG_NOSIGNAL);
        return true;
    }

    bool sendAudioData(int clientSocket, Client&) {
        std::vector<int16_t> audioData(4096, 0);
        
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/octet-stream\r\n"
               << "Content-Length: " << (audioData.size() * sizeof(int16_t)) << "\r\n"
               << "\r\n";
        
        send(clientSocket, header.str().c_str(), header.str().length(), MSG_NOSIGNAL);
        send(clientSocket, audioData.data(), audioData.size() * sizeof(int16_t), MSG_NOSIGNAL);
        return true;
    }

    bool sendStatus(int clientSocket) {
        std::ostringstream json;
        json << "{ \"bands\": [ ";
        bool first = true;
        for (const auto& pair : g_bands) {
            if (!first) json << ", ";
            first = false;
            json << "{\"name\":\"" << pair.first << "\", "
                 << "\"centerFreq\":" << pair.second.centerFreq << ", "
                 << "\"sampleRate\":" << pair.second.sampleRate << ", "
                 << "\"enabled\":" << (pair.second.enabled ? "true" : "false") << "}";
        }
        json << "], \"users\": " << g_clients.size() << "}";
        
        std::string response = json.str();
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/json\r\n"
               << "Content-Length: " << response.length() << "\r\n"
               << "\r\n";
        
        send(clientSocket, header.str().c_str(), header.str().length(), MSG_NOSIGNAL);
        send(clientSocket, response.c_str(), response.length(), MSG_NOSIGNAL);
        return true;
    }

    void sendErrorResponse(int clientSocket, int code, const std::string& message) {
        std::ostringstream response;
        response << "HTTP/1.1 " << code << " " << message << "\r\n"
                 << "Content-Type: text/html\r\n"
                 << "Content-Length: " << message.length() << "\r\n"
                 << "\r\n" << message;
        send(clientSocket, response.str().c_str(), response.str().length(), MSG_NOSIGNAL);
    }
};

int main(int argc, char* argv[]) {
    std::cout << "WebSDR Server starting..." << std::endl;
    std::cout << "Multi-band SDR server for RX888" << std::endl;
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::string configFile = "cfg/websdr.cfg";
    if (argc > 1) configFile = argv[1];
    if (!parseConfig(configFile)) {
        std::cerr << "Failed to load configuration" << std::endl;
        return 1;
    }

    std::cout << "\nConfigured bands:" << std::endl;
    for (const auto& pair : g_bands) {
        std::cout << "  " << pair.first << ": " 
                  << pair.second.centerFreq << " kHz, "
                  << pair.second.sampleRate << " sps, FIFO: "
                  << pair.second.device << std::endl;
    }
    std::cout << std::endl;

    fftwf_init_threads();

    WebSDRServer server;
    if (!server.start()) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "Server running. Press Ctrl+C to stop." << std::endl;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
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