//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "decoder_hevc.h"

#include "../../transcoder_private.h"
#include "../codec_utilities.h"
#include "base/info/application.h"

bool DecoderHEVC::Configure(std::shared_ptr<TranscodeContext> context)
{
	if (TranscodeDecoder::Configure(context) == false)
	{
		return false;
	}

	AVCodec *_codec = ::avcodec_find_decoder(GetCodecID());
	if (_codec == nullptr)
	{
		logte("Codec not found: %s (%d)", ::avcodec_get_name(GetCodecID()), GetCodecID());
		return false;
	}

	// create codec context
	_context = ::avcodec_alloc_context3(_codec);
	if (_context == nullptr)
	{
		logte("Could not allocate codec context for %s (%d)", ::avcodec_get_name(GetCodecID()), GetCodecID());
		return false;
	}

	_context->time_base = TimebaseToAVRational(GetTimebase());

	if (::avcodec_open2(_context, _codec, nullptr) < 0)
	{
        // close codec context to prevent definetly memory leak issue
        ::avcodec_close(_context);

		logte("Could not open codec: %s (%d)", ::avcodec_get_name(GetCodecID()), GetCodecID());
		return false;
	}

	// Create packet parser
	_parser = ::av_parser_init(_codec->id);
	if (_parser == nullptr)
	{
        // close codec context to prevent definetly memory leak issue
        ::avcodec_close(_context);

		logte("Parser not found");
		return false;
	}

	_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

	// Generates a thread that reads and encodes frames in the input_buffer queue and places them in the output queue.
	try
	{
		_kill_flag = false;

		_thread_work = std::thread(&TranscodeDecoder::ThreadDecode, this);
		pthread_setname_np(_thread_work.native_handle(), ov::String::FormatString("Dec%s", avcodec_get_name(GetCodecID())).CStr());
	}
	catch (const std::system_error &e)
	{
        // close codec context to prevent definetly memory leak issue
        ::avcodec_close(_context);

		logte("Failed to start decoder thread");
		_kill_flag = true;
		return false;
	}

	return true;
}

void DecoderHEVC::ThreadDecode()
{
	while (!_kill_flag)
	{
		auto obj = _input_buffer.Dequeue();
		if (obj.has_value() == false)
		{
			// logte("An error occurred while dequeue : no data");
			continue;
		}

		auto buffer = std::move(obj.value());

		auto packet_data = buffer->GetData();

		int64_t remained = packet_data->GetLength();
		off_t offset = 0LL;
		int64_t pts = (buffer->GetPts() == -1LL) ? AV_NOPTS_VALUE : buffer->GetPts();
		int64_t dts = (buffer->GetDts() == -1LL) ? AV_NOPTS_VALUE : buffer->GetDts();
		auto data = packet_data->GetDataAs<uint8_t>();

		while (remained > 0)
		{
			::av_init_packet(_pkt);

			int parsed_size = ::av_parser_parse2(_parser, _context, &_pkt->data, &_pkt->size,
												 data + offset, static_cast<int>(remained), pts, dts, 0);

			if (parsed_size < 0)
			{
				logte("An error occurred while parsing: %d", parsed_size);
				break;
			}

			if (_pkt->size > 0)
			{
				_pkt->pts = _parser->pts;
				_pkt->dts = _parser->dts;

				_pkt->flags = (_parser->key_frame == 1) ? AV_PKT_FLAG_KEY : 0;

				int ret = ::avcodec_send_packet(_context, _pkt);

				if (ret == AVERROR(EAGAIN))
				{
					// Need more data
				}
				else if (ret == AVERROR_EOF)
				{
					logte("An error occurred while sending a packet for decoding: End of file (%d)", ret);
					break;
				}
				else if (ret == AVERROR(EINVAL))
				{
					logte("An error occurred while sending a packet for decoding: Invalid argument (%d)", ret);
					break;
				}
				else if (ret == AVERROR(ENOMEM))
				{
					logte("An error occurred while sending a packet for decoding: No memory (%d)", ret);
					break;
				}
				else if (ret == AVERROR_INVALIDDATA)
				{
					// If only SPS/PPS Nalunit is entered in the decoder, an invalid data error occurs.
					// There is no particular problem.
					logtd("Invalid data found when processing input (%d)", ret);
					break;
				}
				else if (ret < 0)
				{
					char err_msg[1024];
					av_strerror(ret, err_msg, sizeof(err_msg));
					logte("An error occurred while sending a packet for decoding: Unhandled error (%d:%s) ", ret, err_msg);
				}
			}

			OV_ASSERT(
				remained >= parsed_size,
				"Current data size MUST greater than parsed_size, but data size: %ld, parsed_size: %ld",
				remained, parsed_size);

			offset += parsed_size;
			remained -= parsed_size;
		}

		/////////////////////////////////////////////////////////////////////////////////////////////////////////
		while (true)
		{
			// Check the decoded frame is available
			int ret = ::avcodec_receive_frame(_context, _frame);

			if (ret == AVERROR(EAGAIN))
			{
				break;
			}
			else if (ret == AVERROR_EOF)
			{
				logtw("Error receiving a packet for decoding : AVERROR_EOF");
				break;
			}
			else if (ret < 0)
			{
				logte("Error receiving a packet for decoding : %d", ret);
				break;
			}
			else
			{
				bool need_to_change_notify = false;

				// Update codec information if needed
				if (_change_format == false)
				{
					ret = ::avcodec_parameters_from_context(_codec_par, _context);

					if (ret == 0)
					{
						auto codec_info = ShowCodecParameters(_context, _codec_par);
						logti("[%s/%s(%u)] input stream information: %s",
							  _stream_info.GetApplicationInfo().GetName().CStr(), _stream_info.GetName().CStr(), _stream_info.GetId(), codec_info.CStr());

						_change_format = true;

						// If the format is changed, notify to another module
						need_to_change_notify = true;
					}
					else
					{
						logte("Could not obtain codec paramters from context %p", _context);
					}
				}

				// TODO(soulk) : Reduce memory copy overhead. Memory copy can be removed in the Decoder -> Filter step.
				auto decoded_frame = TranscoderUtilities::ConvertToMediaFrame(cmn::MediaType::Video, _frame);
				if (decoded_frame == nullptr)
				{
					continue;
				}
				decoded_frame->SetDuration(TranscoderUtilities::GetDurationPerFrame(cmn::MediaType::Video, _input_context));

				::av_frame_unref(_frame);

				TranscodeResult result = need_to_change_notify ? TranscodeResult::FormatChanged : TranscodeResult::DataReady;

				_output_buffer.Enqueue(std::move(decoded_frame));

				OnCompleteHandler(result, _track_id);
			}
		}
	}
}

std::shared_ptr<MediaFrame> DecoderHEVC::RecvBuffer(TranscodeResult *result)
{
	if (!_output_buffer.IsEmpty())
	{
		*result = TranscodeResult::DataReady;

		auto obj = _output_buffer.Dequeue();
		if (obj.has_value())
		{
			return obj.value();
		}
	}

	*result = TranscodeResult::NoData;
	return nullptr;
}