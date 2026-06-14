// written by bastiaan konings schuiling 2008 - 2014
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "imageloader.hpp"

#include "base/log.hpp"

#include <fstream>

namespace blunted {

  ImageLoader::ImageLoader() : Loader<Surface>() {
  }

  ImageLoader::~ImageLoader() {
  }

  // load file into resource
  void ImageLoader::Load(std::string filename, boost::intrusive_ptr < Resource <Surface> > resource) {
    SDL_Surface *surface = IMG_Load(filename.c_str());
    if (!surface) {
      // Fallback: ASE files reference .jpg/.png but actual textures are .bmp
      std::string ext;
      if (filename.length() > 4) ext = filename.substr(filename.length() - 4);
      if (ext == ".jpg" || ext == ".png") {
        std::string bmpFilename = filename.substr(0, filename.length() - 4) + ".bmp";
        surface = IMG_Load(bmpFilename.c_str());
        if (surface) {
          Log(e_Notice, "ImageLoader", "Load", "Fallback: loaded " + bmpFilename + " instead of " + filename);
        }
      }
    }
    if (!surface) {
      Log(e_Warning, "ImageLoader", "Load", "Could not load " + filename + ", using 1x1 black placeholder");
      surface = SDL_CreateRGBSurface(0, 1, 1, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
      if (surface) {
        SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 0, 0, 0, 255));
      } else {
        Log(e_FatalError, "ImageLoader", "Load", "Could not create placeholder surface");
      }
    }
    resource->GetResource()->SetData(surface);
  }

}
