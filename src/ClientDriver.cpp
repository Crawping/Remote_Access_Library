#include "ClientDriver.h"
#include "turbojpeg.h"
#include "IClientDriver.h"
#include "Shapes.h"
#include "Image.h"
#include "Input.h"
#include "Logging.h"
#include "Configs.h"
#include "SCCommon.h"

#include "WS_Lite.h"

#include <assert.h>
#include <mutex>

namespace SL {
	namespace RAT {

		class ClientDriverImpl {

			IClientDriver* IClientDriver_;
			std::shared_ptr<Client_Config> Config_;
            WS_LITE::WSClient h;
			Point LastMousePosition_;
			std::shared_ptr<WS_LITE::IWSocket> Socket_;
			std::thread Runner;
			std::mutex outputbufferLock;
			std::vector<unsigned char> outputbuffer;

			void MouseImage(const unsigned char* data, size_t len) {
				assert(len >= sizeof(Rect));
				Image img(*reinterpret_cast<const Rect*>(data), data + sizeof(Rect), len - sizeof(Rect));
				assert(len >= sizeof(Rect) + (img.Rect_.Width * img.Rect_.Height * PixelStride));
				IClientDriver_->onReceive_MouseImage(img);
			}

			void MousePos(const unsigned char* data, size_t len) {
				assert(len == sizeof(Point));
				IClientDriver_->onReceive_MousePos(reinterpret_cast<const Point*>(data));
			}
			void MonitorInfo(const unsigned char* data, size_t len) {
				auto num = len / sizeof(Screen_Capture::Monitor);
				assert(num * sizeof(Screen_Capture::Monitor) == len);
				IClientDriver_->onReceive_Monitors((Screen_Capture::Monitor*)data, num);
			}
			void ScreenImage(const unsigned char* data, size_t len) {
				assert(len >= sizeof(Rect));

				auto jpegDecompressor = tjInitDecompress();

				int jpegSubsamp(0), outwidth(0), outheight(0);

				auto src = (unsigned char*)data;
				auto monitor_id = 0;
				memcpy(&monitor_id, src, sizeof(monitor_id));
				src += sizeof(monitor_id);
				Rect rect;
				memcpy(&rect, src, sizeof(rect));
				src += sizeof(rect);

				if (tjDecompressHeader2(jpegDecompressor, src, static_cast<unsigned long>(len - sizeof(Rect)), &outwidth, &outheight, &jpegSubsamp) == -1) {
					SL_RAT_LOG(Logging_Levels::ERROR_log_level, tjGetErrorStr());
				}
				std::lock_guard<std::mutex> lock(outputbufferLock);
				outputbuffer.reserve(outwidth* outheight * PixelStride);

				if (tjDecompress2(jpegDecompressor, src, static_cast<unsigned long>(len - sizeof(Rect)), (unsigned char*)outputbuffer.data(), outwidth, 0, outheight, TJPF_RGBX, TJFLAG_FASTDCT | TJFLAG_NOREALLOC) == -1) {
					SL_RAT_LOG(Logging_Levels::ERROR_log_level, tjGetErrorStr());
				}
				Image img(rect, outputbuffer.data(), outwidth* outheight * PixelStride);
				assert(outwidth == img.Rect_.Width && outheight == img.Rect_.Height);

				IClientDriver_->onReceive_ImageDif(img, monitor_id);
				tjDestroy(jpegDecompressor);
			}


		public:
			ClientDriverImpl(IClientDriver* r) :
				IClientDriver_(r) {

			}
			void Connect(std::shared_ptr<Client_Config> config, const char* dst_host) {
				Config_ = config;
                auto hc = std::string("ws://") + std::string(dst_host);
                h = SL::WS_LITE::CreateContext(SL::WS_LITE::ThreadCount(1))
                    .CreateClient()
                    .onConnection([&](const std::shared_ptr<SL::WS_LITE::IWSocket>& socket, const std::unordered_map<std::string, std::string>& header) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "onConnection ");
                    Socket_ = socket;
					IClientDriver_->onConnection(socket);
                }).onDisconnection([&](const std::shared_ptr<SL::WS_LITE::IWSocket>& socket, unsigned short code, const std::string& msg) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "onDisconnection ");
                    Socket_.reset();
                    IClientDriver_->onDisconnection(socket, code, msg);
                }).onMessage([&](const std::shared_ptr<SL::WS_LITE::IWSocket>& socket, const SL::WS_LITE::WSMessage& message) {
			
					auto p = *reinterpret_cast<const PACKET_TYPES*>(message.data);
					//SL_RAT_LOG(Logging_Levels::INFO_log_level, "onMessage "<<(unsigned int)p);
					switch (p) {
					case PACKET_TYPES::MONITORINFO:
						MonitorInfo( message.data + sizeof(p), message.len - sizeof(p));
						break;
					case PACKET_TYPES::SCREENIMAGEDIF:
						ScreenImage( message.data + sizeof(p), message.len - sizeof(p));
						break;
					case PACKET_TYPES::MOUSEIMAGE:
						MouseImage( message.data + sizeof(p), message.len - sizeof(p));
						break;
					case PACKET_TYPES::MOUSEPOS:
						MousePos( message.data + sizeof(p), message.len - sizeof(p));
						break;
					case PACKET_TYPES::CLIPBOARDTEXTEVENT:
						IClientDriver_->onReceive_ClipboardText(message.data + sizeof(p), message.len - sizeof(p));
						break;
					default:
						IClientDriver_->onMessage(socket, message);//pass up the chain
						break;
					}
					
				}).connect(hc.c_str(), config->WebSocketTLSLPort);

			}

			virtual ~ClientDriverImpl() {
				
			}


			void SendMouse(const MouseEvent& m) {
				if (!Socket_) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "SendMouse called on a socket that is not open yet");
					return;
				}
				if (Socket_->is_loopback()) return;//dont send mouse info to ourselfs as this will cause a loop
				//do checks to prevent sending redundant mouse information about its position
				if (m.EventData == NO_EVENTDATA && LastMousePosition_ == m.Pos && m.PressData == NO_PRESS_DATA && m.ScrollDelta == 0) {
					return;//already did this event
				}
				LastMousePosition_ = m.Pos;
				auto ptype = PACKET_TYPES::MOUSEEVENT;
				const auto size = sizeof(ptype) + sizeof(m);
                auto ptr(std::shared_ptr<unsigned char>(new unsigned char[size], [](auto* p) { delete[] p; }));
                *reinterpret_cast<PACKET_TYPES*>(ptr.get()) = ptype;
                memcpy(ptr.get() + sizeof(ptype), &m, sizeof(m));

                SL::WS_LITE::WSMessage buf;
                buf.code = WS_LITE::OpCode::BINARY;
                buf.Buffer = ptr;
                buf.len = size;
                buf.data = ptr.get();
                Socket_->send(buf, false);
			}
			void SendKey(const KeyEvent & m) {
				if (!Socket_) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "SendKey called on a socket that is not open yet");
					return;
				}
				auto ptype = PACKET_TYPES::KEYEVENT;
				const auto size = sizeof(ptype) + sizeof(m);
                auto ptr(std::shared_ptr<unsigned char>(new unsigned char[size], [](auto* p) { delete[] p; }));
				*reinterpret_cast<PACKET_TYPES*>(ptr.get()) = ptype;
				memcpy(ptr.get() + sizeof(ptype), &m, sizeof(m));

                SL::WS_LITE::WSMessage buf;
                buf.code = WS_LITE::OpCode::BINARY;
                buf.Buffer = ptr;
                buf.len = size;
                buf.data = ptr.get();
                Socket_->send(buf, false);

			}
			void SendClipboardText(const char* data, unsigned int len) {
				if (!Socket_) {
					SL_RAT_LOG(Logging_Levels::INFO_log_level, "SendClipboardText called on a socket that is not open yet");
					return;
				}
				if (Socket_->is_loopback()) return;//dont send clipboard info to ourselfs as it will cause a loop

				auto ptype = PACKET_TYPES::CLIPBOARDTEXTEVENT;
				auto size = sizeof(ptype) + len;
                auto ptr(std::shared_ptr<unsigned char>(new unsigned char[size], [](auto* p) { delete[] p; }));
				*reinterpret_cast<PACKET_TYPES*>(ptr.get()) = ptype;
				memcpy(ptr.get() + sizeof(ptype), data, len);

                SL::WS_LITE::WSMessage buf;
                buf.code = WS_LITE::OpCode::BINARY;
                buf.Buffer = ptr;
                buf.len = size;
                buf.data = ptr.get();
				Socket_->send(buf, false);

			}
		};

	}
}

SL::RAT::ClientDriver::ClientDriver(IClientDriver * r)
	: ClientDriverImpl_(new ClientDriverImpl(r))
{

}

SL::RAT::ClientDriver::~ClientDriver()
{
	delete ClientDriverImpl_;
}

void SL::RAT::ClientDriver::Connect(std::shared_ptr<Client_Config> config, const char* dst_host)
{
	ClientDriverImpl_->Connect(config, dst_host);
}


void SL::RAT::ClientDriver::SendKey(const KeyEvent & m)
{
	ClientDriverImpl_->SendKey(m);
}

void SL::RAT::ClientDriver::SendMouse(const MouseEvent& m)
{
	ClientDriverImpl_->SendMouse(m);
}


void SL::RAT::ClientDriver::SendClipboardText(const char* data, unsigned int len) {
	return ClientDriverImpl_->SendClipboardText(data, len);
}