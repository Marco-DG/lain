import type { Metadata } from "next";
import { Special_Elite, VT323, Inter } from "next/font/google";
import "./globals.css";

const specialElite = Special_Elite({
  weight: "400",
  subsets: ["latin"],
  variable: "--font-elite",
});

const vt323 = VT323({
  weight: "400",
  subsets: ["latin"],
  variable: "--font-vt",
});

const inter = Inter({
  subsets: ["latin"],
  variable: "--font-inter",
});

export const metadata: Metadata = {
  title: "Lain Programming Language",
  description: "A fast, memory-safe, deterministic, and highly syntactic language.",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body className={`${inter.variable} ${specialElite.variable} ${vt323.variable}`}>
        {children}
      </body>
    </html>
  );
}
