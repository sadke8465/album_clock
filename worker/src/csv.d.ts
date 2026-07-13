// CSV files are imported as raw text via the wrangler [[rules]] Text loader.
declare module "*.csv" {
  const content: string;
  export default content;
}
