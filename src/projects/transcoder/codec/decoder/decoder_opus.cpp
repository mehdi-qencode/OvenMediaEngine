//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "decoder_opus.h"

#include "../../transcoder_private.h"
#include "../codec_utilities.h"
#include "base/info/application.h"

bool DecoderOPUS::Configure(std::shared_ptr<TranscodeContext> context)
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

void DecoderOPUS::ThreadDecode()
{
	bool no_data_to_encode = false;

	while (!_kill_flag)
	{
		/////////////////////////////////////////////////////////////////////
		// Sending a packet to decoder
		/////////////////////////////////////////////////////////////////////
		if (_cur_pkt == nullptr && (_input_buffer.IsEmpty() == false || no_data_to_encode == true))
		{
			auto obj = _input_buffer.Dequeue();
			if (obj.has_value() == false)
			{
				continue;
			}

			no_data_to_encode = false;
			_cur_pkt = std::move(obj.value());
			if (_cur_pkt != nullptr)
			{
				_cur_data = _cur_pkt->GetData();
				_pkt_offset = 0;
			}

			if ((_cur_data == nullptr) || (_cur_data->GetLength() == 0))
			{
				continue;
			}
		}

		if (_cur_data != nullptr)
		{
			while (_cur_data->GetLength() > _pkt_offset)
			{
				int32_t parsed_size = ::av_parser_parse2(
					_parser,
					_context,
					&_pkt->data, &_pkt->size,
					_cur_data->GetDataAs<uint8_t>() + _pkt_offset,
					static_cast<int32_t>(_cur_data->GetLength() - _pkt_offset),
					_cur_pkt->GetPts(), _cur_pkt->GetPts(),
					0);

				// Failed to parsing
				if (parsed_size <= 0)
				{
					logte("Error while parsing\n");
					_cur_pkt = nullptr;
					_cur_data = nullptr;
					_pkt_offset = 0;
					break;
				}

				if (_pkt->size > 0)
				{
					_pkt->pts = _parser->pts;
					_pkt->dts = _parser->dts;

					int ret = ::avcodec_send_packet(_context, _pkt);

					if (ret == AVERROR(EAGAIN))
					{
						// Need more data
						// *result = TranscodeResult::Again;
						break;
					}
					else if (ret == AVERROR_EOF)
					{
						logte("Error sending a packet for decoding : AVERROR_EOF");
						break;
					}
					else if (ret == AVERROR(EINVAL))
					{
						logte("Error sending a packet for decoding : AVERROR(EINVAL)");
						break;
					}
					else if (ret == AVERROR(ENOMEM))
					{
						logte("Error sending a packet for decoding : AVERROR(ENOMEM)");
						break;
					}
					else if (ret < 0)
					{
						char err_msg[1024];
						av_strerror(ret, err_msg, sizeof(err_msg));
						logte("An error occurred while sending a packet for decoding: Unhandled error (%d:%s) ", ret, err_msg);
					}
				}

				if (parsed_size > 0)
				{
					OV_ASSERT(_cur_data->GetLength() >= (size_t)parsed_size, "Current data size MUST greater than parsed_size, but data size: %ld, parsed_size: %ld", _cur_data->GetLength(), parsed_size);

					_pkt_offset += parsed_size;
				}

				break;
			}

			if (_cur_data->GetLength() <= _pkt_offset)
			{
				_cur_pkt = nullptr;
				_cur_data = nullptr;
				_pkt_offset = 0;
			}
		}

		/////////////////////////////////////////////////////////////////////
		// Receive a frame from decoder
		/////////////////////////////////////////////////////////////////////
		// Check the decoded frame is available
		int ret = ::avcodec_receive_frame(_context, _frame);
		if (ret == AVERROR(EAGAIN))
		{
			no_data_to_encode = true;
			continue;
		}
		else if (ret == AVERROR_EOF)
		{
			logte("Error receiving a packet for decoding : AVERROR_EOF");
			continue;
		}
		else if (ret < 0)
		{
			logte("Error receiving a packet for decoding : %d", ret);
			continue;
		}
		else
		{
			bool need_to_change_notify = false;

			// Update codec informations if needed
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

			_decoded_frame_num++;

			// TODO(soulk) : Reduce memory copy overhead. Memory copy can be removed in the Decoder -> Filter step.
			auto output_frame = TranscoderUtilities::ConvertToMediaFrame(cmn::MediaType::Audio, _frame);
			if (output_frame == nullptr)
				continue;

			output_frame->SetDuration(TranscoderUtilities::GetDurationPerFrame(cmn::MediaType::Audio, _input_context, _frame));

			// If the decoded audio frame does not have a PTS, Increase frame duration time in PTS of previous frame
			output_frame->SetPts(static_cast<int64_t>((_frame->pts == AV_NOPTS_VALUE) ? _last_pkt_pts + output_frame->GetDuration() : _frame->pts));
			_last_pkt_pts = output_frame->GetPts();

			::av_frame_unref(_frame);

			// Return 1, if notification is required
			TranscodeResult result = need_to_change_notify ? TranscodeResult::FormatChanged : TranscodeResult::DataReady;

			_output_buffer.Enqueue(std::move(output_frame));
			OnCompleteHandler(result, _track_id);
		}
	}
}

std::shared_ptr<MediaFrame> DecoderOPUS::RecvBuffer(TranscodeResult *result)
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
