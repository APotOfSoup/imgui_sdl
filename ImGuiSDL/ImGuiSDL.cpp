#include "SDL.h"
#undef main

#include "SDL_image.h"
#include "SDL2_gfxPrimitives.h"
#include "imgui.h"
#include "imgui_sw.hpp"

#include <map>
#include <vector>
#include <iostream>
#include <algorithm>

namespace ImGuiSDL
{
	struct fixed
	{
		int32_t value;

		static constexpr unsigned shift = 16;

		explicit fixed(int32_t v = 0) : value(v) { }
		explicit fixed(float v) : value(v * (1 << shift) + 0.5f) { }
		explicit fixed(double v) : value(v * (1 << shift) + 0.5) { }

		double to_double() const { return static_cast<double>(value) / (1 << shift); }

		fixed operator+(const fixed& v) const { return fixed(value + v.value); }
		fixed operator-(const fixed& v) const { return fixed(value - v.value); }
		fixed operator*(const fixed& v) const { return fixed(static_cast<int32_t>((static_cast<int64_t>(value) * v.value) >> shift)); }
		fixed operator/(const fixed& v) const { return fixed(static_cast<int32_t>((static_cast<int64_t>(value) << shift) / v.value)); }
		bool operator==(const fixed& v) const { return value == v.value; }
		bool operator<(const fixed& v) const { return to_double() < v.to_double(); }
	};

	struct FixedVector
	{
		fixed X, Y;

		FixedVector(float x, float y) : X(x), Y(y) { }
		FixedVector(fixed x, fixed y) : X(x), Y(y) { }
	};

	struct FixedVertex
	{
		const FixedVector Position;
		const FixedVector TextureCoordinate;
		const uint32_t Color;

		explicit FixedVertex(const ImDrawVert& vert)
			: Position(vert.pos.x, vert.pos.y), TextureCoordinate(vert.uv.x, vert.uv.y), Color(vert.col) { }
	};

	struct Line
	{
		const fixed XCoefficient;
		const fixed YCoefficient;
		const fixed Constant;
		const bool Tie;

		Line(fixed x0, fixed y0, fixed x1, fixed y1)
			: XCoefficient(y0 - y1), 
			YCoefficient(x1 - x0), 
			Constant(fixed(-0.5f) * (XCoefficient * (x0 + x1) + YCoefficient * (y0 + y1))),
			Tie(XCoefficient.value != 0 ? XCoefficient.value > 0 : YCoefficient.value > 0) { }

		fixed Evaluate(fixed x, fixed y) const
		{
			return XCoefficient * x + YCoefficient * y + Constant;
		}

		bool IsInside(fixed x, fixed y) const
		{
			return IsInside(Evaluate(x, y));
		}

		bool IsInside(fixed v) const
		{
			return (v.value > 0 || (v.value == 0 && Tie));
		}
	};

	struct InterpolatedFactorEquation
	{
		const fixed Value0;
		const fixed Value1;
		const fixed Value2;

		const FixedVector& V0;
		const FixedVector& V1;
		const FixedVector& V2;

		const fixed Divisor;

		InterpolatedFactorEquation(fixed value0, fixed value1, fixed value2, const FixedVector& v0, const FixedVector& v1, const FixedVector& v2)
			: Value0(value0), Value1(value1), Value2(value2), V0(v0), V1(v1), V2(v2), 
			  Divisor((V1.Y - V2.Y) * (V0.X - V2.X) + (V2.X - V1.X) * (V0.Y - V2.Y)) { }

		fixed Evaluate(fixed x, fixed y) const
		{
			const fixed w1 = ((V1.Y - V2.Y) * (x - V2.X) + (V2.X - V1.X) * (y - V2.Y)) / Divisor;
			const fixed w2 = ((V2.Y - V0.Y) * (x - V2.X) + (V0.X - V2.X) * (y - V2.Y)) / Divisor;
			const fixed w3 = fixed(1.0) - w1 - w2;

			return w1 * Value0 + w2 * Value1 + w3 * Value2;
		}
	};

	struct Color
	{
		const fixed R, G, B, A;

		explicit Color(uint32_t color)
			: R(((color >> 0) & 0xff) / 255.0f), G(((color >> 8) & 0xff) / 255.0f), B(((color >> 16) & 0xff) / 255.0f), A(((color >> 24) & 0xff) / 255.0f) { }

		Color(fixed r, fixed g, fixed b, fixed a) : R(r), G(g), B(b), A(a) { }

		Color operator*(const Color& c) const { return Color(R * c.R, G * c.G, B * c.B, A * c.A); }

		uint32_t ToInt() const
		{
			return	((static_cast<int>(R.to_double() * 255) & 0xff) << 0) 
				  | ((static_cast<int>(G.to_double() * 255) & 0xff) << 8) 
				  | ((static_cast<int>(B.to_double() * 255) & 0xff) << 16)
				  | ((static_cast<int>(A.to_double() * 255) & 0xff) << 24);
		}
	};

	struct Texture
	{
		SDL_Surface* Surface;
		SDL_Texture* Source;

		Color Sample(float u, float v) const
		{
			const int x = static_cast<int>(roundf(u * (Surface->w - 1) + 0.5f));
			const int y = static_cast<int>(roundf(v * (Surface->h - 1) + 0.5f));

			const int location = y * Surface->w + x;
			assert(location < Surface->w * Surface->h);

			return Color(static_cast<uint32_t*>(Surface->pixels)[location]);
		}
	};

	struct Rect
	{
		fixed MinX, MinY, MaxX, MaxY;
		fixed MinU, MinV, MaxU, MaxV;

		bool IsOnExtreme(const FixedVector& point) const
		{
			return (point.X == MinX || point.X == MaxX) && (point.Y == MinY || point.Y == MaxY);
		}

		bool UsesOnlyColor(const Texture* texture) const
		{
			// TODO: Consider caching the fixed point representation of these.
			const fixed whiteU = fixed(0.5 / texture->Surface->w);
			const fixed whiteV = fixed(0.5 / texture->Surface->h);

			return MinU == MaxU && MinU == whiteU && MinV == MaxV && MaxV == whiteV;
		}
	};

	struct ClipRect
	{
		int X, Y, Width, Height;
	};

	struct Target
	{
		std::vector<uint32_t> Pixels;
		int Width, Height;

		ClipRect Clip;

		SDL_Renderer* Renderer;

		// TODO: Implement an LRU cache and make its size configurable.
		// Caches by UV coordinates and color. The format is Vertex1: X, Y, U, V, Color, Vertex2: X, Y, U, V, Color... Width, Height
		using TextureCacheKey = std::tuple<fixed, fixed, fixed, fixed, uint32_t, fixed, fixed, fixed, fixed, uint32_t, fixed, fixed, fixed, fixed, uint32_t, int, int>;
		std::map<TextureCacheKey, SDL_Texture*> CacheTextures;

		Target(int width, int height, SDL_Renderer* renderer) : Renderer(renderer), Width(width), Height(height) {  }

		void Resize(int width, int height)
		{
			Width = width;
			Height = height;

			for (auto& pair : CacheTextures)
			{ 
				SDL_DestroyTexture(pair.second);
			}
			CacheTextures.clear();
		}

		void SetClipRect(const ClipRect& rect)
		{
			Clip = rect;
			const SDL_Rect clip = {
				rect.X,
				rect.Y,
				rect.Width,
				rect.Height
			};
			SDL_RenderSetClipRect(Renderer, &clip);
		}

		void EnableClip() { SetClipRect(Clip); }
		void DisableClip() { SDL_RenderSetClipRect(Renderer, nullptr); }

		void SetAt(float x, float y, const Color& color)
		{
			SDL_SetRenderDrawColor(Renderer, color.R.to_double() * 255, color.G.to_double() * 255, color.B.to_double() * 255, color.A.to_double() * 255);
			SDL_SetRenderDrawBlendMode(Renderer, SDL_BLENDMODE_BLEND);
			SDL_RenderDrawPoint(Renderer, x, y);
		}

		SDL_Texture* MakeTexture(int width, int height)
		{
			SDL_Texture* texture = SDL_CreateTexture(Renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height);
			SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
			return texture;
		}

		void UseAsRenderTarget(SDL_Texture* texture)
		{
			SDL_SetRenderTarget(Renderer, texture);
			if (texture)
			{
				SDL_SetRenderDrawColor(Renderer, 0, 0, 0, 0);
				SDL_RenderClear(Renderer);
			}
		}
	};

	template <typename T> T Min3(T a, T b, T c) { return std::min(a, std::min(b, c)); }
	template <typename T> T Max3(T a, T b, T c) { return std::max(a, std::max(b, c)); }

	Rect CalculateBoundingBox(const FixedVertex& v0, const FixedVertex& v1, const FixedVertex& v2)
	{
		// TODO: This doesn't account for flipped UVs.
		return Rect{ 
			Min3(v0.Position.X, v1.Position.X, v2.Position.X),
			Min3(v0.Position.Y, v1.Position.Y, v2.Position.Y),
			Max3(v0.Position.X, v1.Position.X, v2.Position.X),
			Max3(v0.Position.Y, v1.Position.Y, v2.Position.Y),
			Min3(v0.TextureCoordinate.X, v1.TextureCoordinate.X, v2.TextureCoordinate.X),
			Min3(v0.TextureCoordinate.Y, v1.TextureCoordinate.Y, v2.TextureCoordinate.Y),
			Max3(v0.TextureCoordinate.X, v1.TextureCoordinate.X, v2.TextureCoordinate.X),
			Max3(v0.TextureCoordinate.Y, v1.TextureCoordinate.Y, v2.TextureCoordinate.Y)
		};
	}

	static constexpr int CacheTexturePadding = 2;

	void DrawTriangle(Target& target, const FixedVertex& v0, const FixedVertex& v1, const FixedVertex& v2, const Texture* texture, const Rect& bounding)
	{
		// TODO: Figure out how to actually position this stuff, looks offset.
		const SDL_Rect destination = {
			static_cast<int>(bounding.MinX.to_double()) - CacheTexturePadding,
			static_cast<int>(bounding.MinY.to_double()) - CacheTexturePadding,
			static_cast<int>((bounding.MaxX - bounding.MinX).to_double()) + 2 * CacheTexturePadding,
			static_cast<int>((bounding.MaxY - bounding.MinY).to_double()) + 2 * CacheTexturePadding
		};

		const FixedVector offset0 = FixedVector(v0.Position.X - bounding.MinX, v0.Position.Y - bounding.MinY);
		const FixedVector offset1 = FixedVector(v1.Position.X - bounding.MinX, v1.Position.Y - bounding.MinY);
		const FixedVector offset2 = FixedVector(v2.Position.X - bounding.MinX, v2.Position.Y - bounding.MinY);

		// Constructs a cache key that we can use to see if there's a cached version of what we're about to render,
		// or if there doesn't exist anything, we create a cache item.
		const Target::TextureCacheKey key = std::make_tuple(
			offset0.X, offset0.Y, v0.TextureCoordinate.X, v0.TextureCoordinate.Y, v0.Color,
			offset1.X, offset1.Y, v1.TextureCoordinate.X, v1.TextureCoordinate.Y, v1.Color,
			offset2.X, offset2.Y, v2.TextureCoordinate.X, v2.TextureCoordinate.Y, v2.Color,
			destination.w, destination.h);
		if (target.CacheTextures.count(key) > 0)
		{
			// TODO: Do we use this cache more than once per frame, per texture?

			SDL_RenderCopy(target.Renderer, target.CacheTextures.at(key), nullptr, &destination);

			return;
		}

		const Line line0(v0.Position.X, v0.Position.Y, v1.Position.X, v1.Position.Y);
		const Line line1(v1.Position.X, v1.Position.Y, v2.Position.X, v2.Position.Y);
		const Line line2(v2.Position.X, v2.Position.Y, v0.Position.X, v0.Position.Y);

		const Color color0 = Color(v0.Color);
		const Color color1 = Color(v1.Color);
		const Color color2 = Color(v2.Color);

		const InterpolatedFactorEquation textureU(v0.TextureCoordinate.X, v1.TextureCoordinate.X, v2.TextureCoordinate.X, v0.Position, v1.Position, v2.Position);
		const InterpolatedFactorEquation textureV(v0.TextureCoordinate.Y, v1.TextureCoordinate.Y, v2.TextureCoordinate.Y, v0.Position, v1.Position, v2.Position);

		const InterpolatedFactorEquation shadeR(color0.R, color1.R, color2.R, v0.Position, v1.Position, v2.Position);
		const InterpolatedFactorEquation shadeG(color0.G, color1.G, color2.G, v0.Position, v1.Position, v2.Position);
		const InterpolatedFactorEquation shadeB(color0.B, color1.B, color2.B, v0.Position, v1.Position, v2.Position);
		const InterpolatedFactorEquation shadeA(color0.A, color1.A, color2.A, v0.Position, v1.Position, v2.Position);

		SDL_Texture* cacheTexture = target.MakeTexture(destination.w, destination.h);
		target.UseAsRenderTarget(cacheTexture);
		target.DisableClip();

		for (int drawY = 0; drawY <= destination.h; drawY += 1)
		{
			for (int drawX = 0; drawX <= destination.w; drawX += 1)
			{
				const fixed sampleX = fixed(drawX + 0.5f) + bounding.MinX;
				const fixed sampleY = fixed(drawY + 0.5f) + bounding.MinY;

				const fixed checkX = sampleX;
				const fixed checkY = sampleY;

				if (line0.IsInside(checkX, checkY) && line1.IsInside(checkX, checkY) && line2.IsInside(checkX, checkY))
				{
					const fixed u = textureU.Evaluate(sampleX, sampleY);
					const fixed v = textureV.Evaluate(sampleX, sampleY);

					// Sample the color from the surface.
					const Color& sampled = texture->Sample(u.to_double(), v.to_double());

					const Color& shade = Color(shadeR.Evaluate(sampleX, sampleY), shadeG.Evaluate(sampleX, sampleY), shadeB.Evaluate(sampleX, sampleY), shadeA.Evaluate(sampleX, sampleY));

					target.SetAt(drawX + CacheTexturePadding, drawY + CacheTexturePadding, sampled * shade);
				}
			}
		}

		target.EnableClip();
		target.UseAsRenderTarget(nullptr);

		SDL_RenderCopy(target.Renderer, cacheTexture, nullptr, &destination);

		target.CacheTextures[key] = cacheTexture;
	}

	void DrawBottomFlatTriangle(Target& target, fixed x0, fixed y0, fixed x1, fixed y1, fixed x2, fixed y2, const Color& color)
	{
		const fixed invSlope0 = (x1 - x0) / (y1 - y0);
		const fixed invSlope1 = (x2 - x0) / (y2 - y0);

		fixed currentX0(x0);
		fixed currentX1(x0);

		for (int scanLineY = y0.to_double(); scanLineY <= y1.to_double(); scanLineY++)
		{
			// Draws a horizontal line slice.
			const double xStart = std::min(currentX0.to_double(), currentX1.to_double());
			const double xEnd = std::max(currentX0.to_double(), currentX1.to_double());
			for (int x = static_cast<int>(xStart); x <= static_cast<int>(xEnd); x++)
			{
				target.SetAt(x + CacheTexturePadding, scanLineY + CacheTexturePadding, color);
			}

			currentX0 = currentX0 + invSlope1;
			currentX1 = currentX1 + invSlope0;
		}
	}

	void DrawTopFlatTriangle(Target& target, fixed x0, fixed y0, fixed x1, fixed y1, fixed x2, fixed y2, const Color& color)
	{
		const fixed invSlope0 = (x2 - x0) / (y2 - y0);
		const fixed invSlope1 = (x2 - x1) / (y2 - y1);

		fixed currentX0(x2);
		fixed currentX1(x2);

		for (int scanLineY = y2.to_double(); scanLineY > y0.to_double(); scanLineY--)
		{
			// Draws a horizontal line slice.
			const double xStart = std::min(currentX0.to_double(), currentX1.to_double());
			const double xEnd = std::max(currentX0.to_double(), currentX1.to_double());
			for (int x = static_cast<int>(xStart); x <= static_cast<int>(xEnd); x++)
			{
				target.SetAt(x + CacheTexturePadding, scanLineY + CacheTexturePadding, color);
			}

			currentX0 = currentX0 - invSlope0;
			currentX1 = currentX1 - invSlope1;
		}
	}

	void DrawUniformColorTriangle(Target& target, const FixedVertex& v0, const FixedVertex& v1, const FixedVertex& v2, const Rect& bounding)
	{
		// TODO: Code duplication from the generic triangle function.

		// TODO: Figure out how to actually position this stuff, looks offset.
		const SDL_Rect destination = {
			static_cast<int>(bounding.MinX.to_double()) - CacheTexturePadding,
			static_cast<int>(bounding.MinY.to_double()) - CacheTexturePadding,
			static_cast<int>((bounding.MaxX - bounding.MinX).to_double()) + 2 * CacheTexturePadding,
			static_cast<int>((bounding.MaxY - bounding.MinY).to_double()) + 2 * CacheTexturePadding
		};

		const FixedVector offset0 = FixedVector(v0.Position.X - bounding.MinX, v0.Position.Y - bounding.MinY);
		const FixedVector offset1 = FixedVector(v1.Position.X - bounding.MinX, v1.Position.Y - bounding.MinY);
		const FixedVector offset2 = FixedVector(v2.Position.X - bounding.MinX, v2.Position.Y - bounding.MinY);

		// Constructs a cache key that we can use to see if there's a cached version of what we're about to render,
		// or if there doesn't exist anything, we create a cache item.
		const Target::TextureCacheKey key = std::make_tuple(
			offset0.X, offset0.Y, v0.TextureCoordinate.X, v0.TextureCoordinate.Y, v0.Color,
			offset1.X, offset1.Y, v1.TextureCoordinate.X, v1.TextureCoordinate.Y, v1.Color,
			offset2.X, offset2.Y, v2.TextureCoordinate.X, v2.TextureCoordinate.Y, v2.Color,
			destination.w, destination.h);
		if (target.CacheTextures.count(key) > 0)
		{
			// TODO: Do we use this cache more than once per frame, per texture?

			SDL_RenderCopy(target.Renderer, target.CacheTextures.at(key), nullptr, &destination);

			return;
		}

		SDL_Texture* cacheTexture = target.MakeTexture(destination.w, destination.h);
		target.UseAsRenderTarget(cacheTexture);
		target.DisableClip();

		// Draw the triangle here.

		const Color color = Color(v0.Color);

		std::vector<FixedVector> vertices = { offset0, offset1, offset2 };
		std::sort(vertices.begin(), vertices.end(), [](const FixedVector& a, const FixedVector& b) { return a.Y.value < b.Y.value; });

		const FixedVector& vertex0 = vertices.at(0);
		const FixedVector& vertex1 = vertices.at(1);
		const FixedVector& vertex2 = vertices.at(2);

		if (vertex1.Y == vertex2.Y)
		{
			DrawBottomFlatTriangle(target, vertex0.X, vertex0.Y, vertex1.X, vertex1.Y, vertex2.X, vertex2.Y, color);
		}
		else if (vertex0.Y == vertex1.Y)
		{
			DrawTopFlatTriangle(target, vertex0.X, vertex0.Y, vertex1.X, vertex1.Y, vertex2.X, vertex2.Y, color);
		}
		else
		{
			const FixedVector vertex3 = FixedVector(
				fixed(vertex0.X + ((vertex1.Y - vertex0.Y) / (vertex2.Y - vertex0.Y)) * (vertex2.X - vertex0.X)), vertex1.Y);

			DrawBottomFlatTriangle(target, vertex0.X, vertex0.Y, vertex1.X, vertex1.Y, vertex3.X, vertex3.Y, color);
			DrawTopFlatTriangle(target, vertex1.X, vertex1.Y, vertex3.X, vertex3.Y, vertex2.X, vertex2.Y, color);
		}

		target.EnableClip();
		target.UseAsRenderTarget(nullptr);

		SDL_RenderCopy(target.Renderer, cacheTexture, nullptr, &destination);

		target.CacheTextures[key] = cacheTexture;
	}

	void DrawRectangle(Target& target, const Rect& bounding, const Texture* texture, const Color& color)
	{
		// We are safe to assume uniform color here, because the caller checks it and and uses the triangle renderer to render those.

		const SDL_Rect destination = {
			bounding.MinX.to_double(),
			bounding.MinY.to_double(),
			(bounding.MaxX - bounding.MinX).to_double(),
			(bounding.MaxY - bounding.MinY).to_double()
		};

		// If the area isn't textured, we can just draw a rectangle with the correct color.
		if (bounding.UsesOnlyColor(texture))
		{
			SDL_SetRenderDrawColor(target.Renderer, color.R.to_double() * 255, color.G.to_double() * 255, color.B.to_double() * 255, color.A.to_double() * 255);
			SDL_RenderFillRect(target.Renderer, &destination);

			return;
		}

		const SDL_Rect source = {
			bounding.MinU.to_double() * texture->Surface->w,
			bounding.MinV.to_double() * texture->Surface->h,
			(bounding.MaxU - bounding.MinU).to_double() * texture->Surface->w,
			(bounding.MaxV - bounding.MinV).to_double() * texture->Surface->h
		};

		SDL_SetTextureColorMod(texture->Source, color.R.to_double() * 255, color.G.to_double() * 255, color.B.to_double() * 255);
		SDL_RenderCopy(target.Renderer, texture->Source, &source, &destination);
	}

	void DoImGuiRender(Target& target, ImDrawData* drawData)
	{
		for (int n = 0; n < drawData->CmdListsCount; n++)
		{
			auto cmdList = drawData->CmdLists[n];
			auto vertexBuffer = cmdList->VtxBuffer;  // vertex buffer generated by ImGui
			auto indexBuffer = cmdList->IdxBuffer.Data;   // index buffer generated by ImGui

			for (int cmd_i = 0; cmd_i < cmdList->CmdBuffer.Size; cmd_i++)
			{
				const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmd_i];

				const ClipRect clipRect = {
					pcmd->ClipRect.x,
					pcmd->ClipRect.y,
					pcmd->ClipRect.z - pcmd->ClipRect.x,
					pcmd->ClipRect.w - pcmd->ClipRect.y
				};
				target.SetClipRect(clipRect);

				if (pcmd->UserCallback) { pcmd->UserCallback(cmdList, pcmd); }
				else
				{
					const Texture* texture = static_cast<const Texture*>(pcmd->TextureId);

					// Loops over triangles.
					for (int i = 0; i + 3 <= pcmd->ElemCount; i += 3)
					{
						const FixedVertex v0 = FixedVertex(vertexBuffer[indexBuffer[i + 0]]);
						const FixedVertex v1 = FixedVertex(vertexBuffer[indexBuffer[i + 1]]);
						const FixedVertex v2 = FixedVertex(vertexBuffer[indexBuffer[i + 2]]);

						const Rect& bounding = CalculateBoundingBox(v0, v1, v2);

						// TODO: Optimize single color triangles.
						const bool isTriangleUniformColor = v0.Color == v1.Color && v1.Color == v2.Color;
						const bool doesTriangleUseOnlyColor = bounding.UsesOnlyColor(texture);

						// Actually, since we render a whole bunch of rectangles, we try to first detect those, and render them more efficiently.
						// How are rectangles detected? It's actually pretty simple: If all 6 vertices lie on the extremes of the bounding box, 
						// it's a rectangle.
						if (i + 6 <= pcmd->ElemCount)
						{
							const FixedVertex v3 = FixedVertex(vertexBuffer[indexBuffer[i + 3]]);
							const FixedVertex v4 = FixedVertex(vertexBuffer[indexBuffer[i + 4]]);
							const FixedVertex v5 = FixedVertex(vertexBuffer[indexBuffer[i + 5]]);

							const bool isUniformColor = isTriangleUniformColor && v2.Color == v3.Color && v3.Color == v4.Color && v4.Color == v5.Color;

							if (isUniformColor
								&& bounding.IsOnExtreme(v0.Position)
								&& bounding.IsOnExtreme(v1.Position)
								&& bounding.IsOnExtreme(v2.Position)
								&& bounding.IsOnExtreme(v3.Position)
								&& bounding.IsOnExtreme(v4.Position)
								&& bounding.IsOnExtreme(v5.Position))
							{
								DrawRectangle(target, bounding, texture, Color(v0.Color));

								i += 3;  // Additional increment.
								continue;
							}
						}

						// TODO: I just really need to figure out subpixel accuracy with fixed point or something.

						//DrawTriangle(target, v0, v1, v2, texture, bounding);

						//continue;

						if (isTriangleUniformColor && doesTriangleUseOnlyColor)
						{
							DrawUniformColorTriangle(target, v0, v1, v2, bounding);
						}
						else
						{
							DrawTriangle(target, v0, v1, v2, texture, bounding);
						}
					}
				}

				indexBuffer += pcmd->ElemCount;
			}
		}
	}
};

int main()
{
	SDL_Init(SDL_INIT_EVERYTHING);

	SDL_Window* window = SDL_CreateWindow("SDL2 ImGui Renderer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_RESIZABLE);
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize.x = 800.0f;
	io.DisplaySize.y = 600.0f;
	ImGui::GetStyle().WindowRounding = 0.0f;
	ImGui::GetStyle().AntiAliasedFill = false;
	ImGui::GetStyle().AntiAliasedLines = false;

	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	// TODO: At this points you've got the texture data and you need to upload that your your graphic system:
	uint32_t rmask, gmask, bmask, amask;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	rmask = 0xff000000;
	gmask = 0x00ff0000;
	bmask = 0x0000ff00;
	amask = 0x000000ff;
#else
	rmask = 0x000000ff;
	gmask = 0x0000ff00;
	bmask = 0x00ff0000;
	amask = 0xff000000;
#endif

	SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(pixels, width, height, 32, 4 * width, rmask, gmask, bmask, amask);
	ImGuiSDL::Texture texture{ surface, SDL_CreateTextureFromSurface(renderer, surface) };
	io.Fonts->TexID = (void*)&texture;

	ImGuiSDL::Target target(800, 600, renderer);

	SDL_Texture* sheet = SDL_CreateTextureFromSurface(renderer, texture.Surface);

	bool run = true;
	while (run)
	{
		int wheel = 0;

		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			if (e.type == SDL_QUIT) run = false;
			else if (e.type == SDL_WINDOWEVENT)
			{
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
				{
					io.DisplaySize.x = static_cast<float>(e.window.data1);
					io.DisplaySize.y = static_cast<float>(e.window.data2);

					target.Resize(e.window.data1, e.window.data2);
				}
			}
			else if (e.	type == SDL_MOUSEWHEEL)
			{
				wheel = e.wheel.y;
			}
		}

		int mouseX, mouseY;
		const int buttons = SDL_GetMouseState(&mouseX, &mouseY);

		// Setup low-level inputs (e.g. on Win32, GetKeyboardState(), or write to those fields from your Windows message loop handlers, etc.)
		ImGuiIO& io = ImGui::GetIO();
		io.DeltaTime = 1.0f / 60.0f;
		io.MousePos = ImVec2(static_cast<float>(mouseX), static_cast<float>(mouseY));
		io.MouseDown[0] = buttons & SDL_BUTTON(SDL_BUTTON_LEFT);
		io.MouseDown[1] = buttons & SDL_BUTTON(SDL_BUTTON_RIGHT);
		io.MouseWheel = static_cast<float>(wheel);

		ImGui::NewFrame();

		ImGui::ShowDemoWindow();

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
		SDL_RenderClear(renderer);

		ImGui::Render();
		ImGuiSDL::DoImGuiRender(target, ImGui::GetDrawData());

		SDL_RenderPresent(renderer);
	}

	SDL_FreeSurface(texture.Surface);

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);

	ImGui::DestroyContext();

	return 0;
}