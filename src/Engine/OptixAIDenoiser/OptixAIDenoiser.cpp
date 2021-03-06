#include <Engine/Viewer/OptixAIDenoiser.h>

#include <Basic/Image.h>
#include <Basic/Timer.h>

using namespace Ubpa;

using namespace std;

#ifdef USE_DENOISE
#undef min
#undef max
#define NOMINMAX
#include <optix_world.h>

optix::Context optix_context;
optix::Buffer input_buffer;
optix::Buffer output_buffer;
optix::PostprocessingStage denoiserStage;
optix::CommandList commandList;

void jobStart(int width, int height, float blend)
{
	// Create optix image buffers
	optix_context = optix::Context::create();

	input_buffer = optix_context->createBuffer(RT_BUFFER_INPUT, RT_FORMAT_FLOAT4, width, height);
	output_buffer = optix_context->createBuffer(RT_BUFFER_OUTPUT, RT_FORMAT_FLOAT4, width, height);

	// Setup the optix denoiser post processing stage
	denoiserStage = optix_context->createBuiltinPostProcessingStage("DLDenoiser");
	denoiserStage->declareVariable("input_buffer")->set(input_buffer);
	denoiserStage->declareVariable("output_buffer")->set(output_buffer);
	denoiserStage->declareVariable("blend")->setFloat(blend);

	// Add the denoiser to the new optix command list
	commandList = optix_context->createCommandList();
	//commandList->appendLaunch(0, width, height);
	commandList->appendPostprocessingStage(denoiserStage, width, height);
	commandList->finalize();
	// Compile context. I'm not sure if this is needed given there is no megakernal?
	optix_context->validate();
	optix_context->compile();
}

void jobComplete()
{
	denoiserStage->destroy();
	commandList->destroy();

	input_buffer->destroy();
	output_buffer->destroy();
	optix_context->destroy();
}

void denoiseImplement(float * img, int w, int h, float blend, bool is_batch)
{
	// Copy all image data to the gpu buffers
	memcpy(input_buffer->map(), img, sizeof(float) * 4 * w * h);
	input_buffer->unmap();

	// Execute denoise
	commandList->execute();

	// Copy denoised image back to the cpu
	memcpy(img, output_buffer->map(), sizeof(float) * 4 * w * h);
	output_buffer->unmap();
}
#endif // USE_DENOISE

void OptixAIDenoiser::Denoise(Ptr<Image> img) {
#ifdef USE_DENOISE
	Timer timer;

	int w = img->GetWidth();
	int h = img->GetHeight();
	int c = img->GetChannel();

	cout << "Width : " << w << ", ";
	cout << "Height : " << h << ", ";
	cout << "Channel : " << c << endl;

	std::cout << "Denoising..." << std::endl;
	timer.Start();
	jobStart(w, h, 0.05f);
	if (c == 3) {
		float * data = new float[w*h * 4];
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				int base = (y*w + x) * 4;
				for (int i = 0; i < 3; i++)
					data[base + i] = img->At(x, y, i);
				data[base + 3] = 1.f;
			}
		}
		denoiseImplement(data, w, h, 0.05f, false);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				int base = (y*w + x) * 4;
				img->SetPixel(x, y, rgbf(
					data[base + 0],
					data[base + 1],
					data[base + 2]
				));
			}
		}
		delete[] data;
	}
	else if (c == 4)
		denoiseImplement(img->GetData(), w, h, 0.05f, false);
	else
		printf("ERROR: img 's channel is not 3 or 4.");

	jobComplete();
	std::cout << "Denoising complete" << std::endl;

	timer.Stop();
	printf("denois time: %f s\n", timer.GetLog(0));
#else
	printf("WARNING::OptixAIDenoiser::Denoise:\n"
		"\t""not support denoise\n");
#endif
}

